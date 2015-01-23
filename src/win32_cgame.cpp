#include <windows.h>
#include <stdint.h>
#include <Xinput.h>
#include <dsound.h>

#define CLAMP(v, min, max) ((v < min) ? min : ((v > max) ? max : v))


struct win32_offscreen_buffer
{
	BITMAPINFO info;
	void *memory;
	int height;
	int width;
	int pixelByteSize;
	int pitch;
};

static bool g_bRunning;
static win32_offscreen_buffer g_buffer;
static LPDIRECTSOUNDBUFFER g_pSoundBuffer = 0;

struct win32_window_size
{
	int width;
	int height;
};

#define X_INPUT_GET_STATE(name) DWORD WINAPI name( DWORD dwUserIndex, XINPUT_STATE *pState )
typedef X_INPUT_GET_STATE(xinput_get_state);
X_INPUT_GET_STATE(XInputGetStateStub) { return ERROR_DEVICE_NOT_CONNECTED; }
static xinput_get_state *XInputGetState_ = XInputGetStateStub;
#define XInputGetState XInputGetState_


#define X_INPUT_SET_STATE(name) DWORD WINAPI name( DWORD dwUserIndex, XINPUT_VIBRATION *pVibration )
typedef X_INPUT_SET_STATE(xinput_set_state);
X_INPUT_SET_STATE(XInputSetStateStub) { return ERROR_DEVICE_NOT_CONNECTED; }
static xinput_set_state *XInputSetState_ = XInputSetStateStub;
#define XInputSetState XInputSetState_

#define D_SOUND_CREATE(name) HRESULT WINAPI name(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter)
typedef D_SOUND_CREATE(direct_sound_create);


void Win32LoadXInput()
{
	HMODULE xInputLib = LoadLibraryA("xinput1_4.dll");

	if (!xInputLib)
		xInputLib = LoadLibraryA("xinput1_3.dll");

	if (xInputLib)
	{
		XInputGetState = (xinput_get_state*)GetProcAddress(xInputLib, "XInputGetState");
		XInputSetState = (xinput_set_state*)GetProcAddress(xInputLib, "XInputSetState");
	}
}

win32_window_size GetWindowSize(HWND hwnd)
{
	RECT clientRect;
	GetClientRect(hwnd, &clientRect);
	win32_window_size size;
	size.width = clientRect.right - clientRect.left;
	size.height = clientRect.bottom - clientRect.top;
	return size;
}

void RenderTestGradient(win32_offscreen_buffer *bufferData, int xOffset, int yOffset)
{
	uint8_t *row = (uint8_t *)bufferData->memory;
	for (int y = 0; y < bufferData->height; ++y)
	{
		uint32_t *pixel = (uint32_t *)row;
		for (int x = 0; x < bufferData->width; ++x)
		{
			uint8_t r = (uint8_t)(x + xOffset);
			uint8_t g = (uint8_t)(x + yOffset);
			uint8_t b = (uint8_t)(y + yOffset);

			*pixel++ = b | (g << 8) | (r << 16);
		}

		row += bufferData->pitch;
	}
}

DWORD Win32WriteSquareWaveToBuffer(uint16_t *pWritePoint, DWORD dwWriteSize, DWORD samplePoint, DWORD feqInterval, DWORD volume)
{
	for (DWORD i = 0; i < dwWriteSize; i += 2)
	{
		samplePoint++;
		int16_t s = (int16_t)(volume * (((samplePoint % feqInterval) < (feqInterval / 2)) ? -1 : 1));
		*pWritePoint++ = s;
	}

	return samplePoint;
}

void Win32ResizeDIBSection(win32_offscreen_buffer *bufferData, int width, int height)
{
	if (bufferData->memory)
	{
		VirtualFree(bufferData->memory, 0, MEM_RELEASE);
	}

	bufferData->width = width;
	bufferData->height = height;
	bufferData->pixelByteSize = 4;

	bufferData->pitch = bufferData->width * bufferData->pixelByteSize;

	bufferData->info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bufferData->info.bmiHeader.biWidth = bufferData->width;
	bufferData->info.bmiHeader.biHeight = -bufferData->height;
	bufferData->info.bmiHeader.biPlanes = 1;
	bufferData->info.bmiHeader.biBitCount = 32;
	bufferData->info.bmiHeader.biCompression = BI_RGB;

	int bitmapMemorySize = bufferData->pixelByteSize * width * height;
	bufferData->memory = VirtualAlloc(0, bitmapMemorySize, MEM_COMMIT, PAGE_READWRITE);

}

void Win32CopyBufferToWindow(win32_offscreen_buffer *bufferData, HDC dc, int windWidth, int windHeight, int x, int y, int width, int height)
{
	int res =  StretchDIBits(
		dc,
		0, 0, windWidth, windHeight,
		0, 0, bufferData->width, bufferData->height,
		bufferData->memory,
		&bufferData->info,
		DIB_RGB_COLORS,
		SRCCOPY
		);
}

void Win32InitDirectSound(HWND hwnd, int32_t samplesPerSecond, int32_t bufferSize)
{

	if (CLAMP(bufferSize, DSBSIZE_MIN, DSBSIZE_MAX) != bufferSize)
	{
		bufferSize = CLAMP(bufferSize, DSBSIZE_MIN, DSBSIZE_MAX);
		OutputDebugStringA("WARNING(DS): Buffersize is not in accepted range (DSBSIZE_MIN < bufferSize < DSBSIZE_MAX)\n");
	}

	// Load the lib
	HMODULE DSoundLibrary = LoadLibraryA("dsound.dll");

	if (DSoundLibrary)
	{
		// Get DirectSound objct
		direct_sound_create *DirectSoundCreate = (direct_sound_create*)GetProcAddress(DSoundLibrary, "DirectSoundCreate");

		LPDIRECTSOUND pDirectSoundObject;

		if (DirectSoundCreate && SUCCEEDED(DirectSoundCreate(0, &pDirectSoundObject, 0)))
		{
			WAVEFORMATEX waveFormatEx = { 0 };
			waveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
			waveFormatEx.nChannels = 2;
			waveFormatEx.nSamplesPerSec = samplesPerSecond;
			waveFormatEx.wBitsPerSample = 16;
			waveFormatEx.nBlockAlign = (waveFormatEx.nChannels * waveFormatEx.wBitsPerSample) / 8;
			waveFormatEx.nAvgBytesPerSec = waveFormatEx.nSamplesPerSec * waveFormatEx.nBlockAlign;
			waveFormatEx.cbSize = 0;

			LPDIRECTSOUNDBUFFER pPrimaryBuffer = 0;


			if (SUCCEEDED(pDirectSoundObject->SetCooperativeLevel(hwnd, DSSCL_PRIORITY)))
			{
				DSBUFFERDESC primaryBufferDesc = { 0 };
				primaryBufferDesc.dwSize = sizeof(DSBUFFERDESC);
				primaryBufferDesc.dwFlags = DSBCAPS_PRIMARYBUFFER;

				LPDIRECTSOUNDBUFFER pPrimaryBuffer;
				HRESULT res = pDirectSoundObject->CreateSoundBuffer(&primaryBufferDesc, &pPrimaryBuffer, 0);
				if (SUCCEEDED(res))
				{
					if (SUCCEEDED(pPrimaryBuffer->SetFormat(&waveFormatEx)))
					{
						DSBUFFERDESC secondaryBufferDesc = { 0 };
						secondaryBufferDesc.dwSize = sizeof(DSBUFFERDESC);
						secondaryBufferDesc.dwFlags = 0;// DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY;
						secondaryBufferDesc.dwBufferBytes = bufferSize;
						secondaryBufferDesc.lpwfxFormat = &waveFormatEx;

						if (SUCCEEDED(pDirectSoundObject->CreateSoundBuffer(&secondaryBufferDesc, &g_pSoundBuffer, 0)))
						{
							OutputDebugStringA("SUCCESS(DS)\n");
						}
					}
					else
					{
						OutputDebugStringA("ERROR(DS): PrimaryBuffyer Set Format Failed\n");
					}
				}
				else
				{
					OutputDebugStringA("ERROR(DS): Failed to create the primary Buffer\n");
				}
			}
			else
			{
				OutputDebugStringA("ERROR(DS): SetCooperativeLevel Failed\n");
			}
		}
		else
		{
			OutputDebugStringA("ERROR(DS): Failed to get DirectSoundCreate handle\n");
		}
	}
	else
	{
		OutputDebugStringA("ERROR(DS): Failed to load dsound.dll\n");
	}
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	LRESULT Result = 0;

	switch (uMsg)
	{
		case WM_SIZE:
		{
			OutputDebugStringA("WM_SIZE\n");
		}
		break;

		case WM_DESTROY:
		{
			g_bRunning = false;
			OutputDebugStringA("WM_DESTROY\n");
		}
		break;

		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
		case WM_KEYDOWN:
		case WM_KEYUP:
		{
			uint64_t VKCode = wParam;
			bool bWasDown = (lParam & (1 << 30)) != 0;
			bool bKeyDown = (lParam & (1 << 31)) == 0;

			if (bKeyDown != bWasDown)
			{
				switch (VKCode)
				{
					case 'W':
					{
					}
					break;
					case 'A':
					{
					}
					break;
					case 'S':
					{
					}
					break;
					case 'D':
					{
					}
					break;
					case 'Q':
					{
					}
					break;
					case 'E':
					{
					}
					break;
					case VK_UP:
					{
					}
					break;
					case VK_LEFT:
					{
					}
					break;
					case VK_DOWN:
					{
					}
					break;
					case VK_RIGHT:
					{
					}
					break;
					case VK_ESCAPE:
					{

					}
					break;
					case VK_SPACE:
					{
					}
					break;
				}
			}

			uint32_t altKeyWasDown = (lParam & (1 << 29));
			if (VKCode == VK_F4 && altKeyWasDown)
			{
				g_bRunning = false;
			}
		}
		break;

		case WM_CLOSE:
		{
			g_bRunning = false;
			OutputDebugStringA("WM_CLOSE\n");
		}
		break;

		case WM_ACTIVATEAPP:
		{
			OutputDebugStringA("WM_ACTIVATEAPP\n");
		}
		break;

		case WM_PAINT:
		{
			PAINTSTRUCT paint;
			HDC dc = BeginPaint(hwnd, &paint);

			win32_window_size wndSize = GetWindowSize(hwnd);

			Win32CopyBufferToWindow(&g_buffer, dc, wndSize.width, wndSize.height, 0, 0, wndSize.width, wndSize.height);

			EndPaint(hwnd, &paint);
		}
		break;

		default:
		{
			Result = DefWindowProc(hwnd, uMsg, wParam, lParam);
		}
		break;
	}

	return Result;
}

int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	Win32LoadXInput();

	WNDCLASSA WindowClass = { 0 };

	WindowClass.style = CS_HREDRAW | CS_VREDRAW;
	WindowClass.lpfnWndProc = &WindowProc;
	WindowClass.hInstance = hInstance;
	//WindowClass.hIcon;
	WindowClass.lpszClassName = "CGameWndClass";

	//Omon Milk

	if(RegisterClassA(&WindowClass))
	{
		HWND hwnd = CreateWindowExA(
			0,
			WindowClass.lpszClassName,
			"Game",
			WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			0,
			0,
			hInstance,
			0);

		if (hwnd)
		{
			int32_t samplesPerSecond = 48000;
			int32_t soundBufferSize = samplesPerSecond * sizeof(int16_t) * 2;
			Win32InitDirectSound(hwnd, samplesPerSecond, soundBufferSize);
			g_pSoundBuffer->Play(0, 0, DSBPLAY_LOOPING);
			win32_window_size wndSize = GetWindowSize(hwnd);
			Win32ResizeDIBSection(&g_buffer, wndSize.width, wndSize.height);

			g_bRunning = true;

			int xOffset = 0;
			int yOffset = 0;
			int squareWaveCounter = 0;

			HDC dc = GetDC(hwnd);

			const float noteD = 440.f;
			const float noteC = 261.6f;
			float note = noteC;

			MSG msg;
			while (g_bRunning)
			{
				while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
				{
					if (msg.message == WM_QUIT)
					{
						g_bRunning = false;
					}
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}

				for (DWORD controllerIndex = 0; controllerIndex < XUSER_MAX_COUNT; ++controllerIndex)
				{
					XINPUT_VIBRATION vibData;
					vibData.wLeftMotorSpeed = xOffset;
					vibData.wRightMotorSpeed = xOffset;
					//XInputSetState(controllerIndex, &vibData);

					XINPUT_STATE state;
					if (XInputGetState(controllerIndex, &state) == ERROR_SUCCESS)
					{
						if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP)
						{
							xOffset += 2;
						}

						if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)
						{

						}

						if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)
						{

						}

						if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)
						{

						}

						note = noteC;
						if (state.Gamepad.wButtons & XINPUT_GAMEPAD_A)
						{
							note = noteD;
						}

						if (state.Gamepad.wButtons & XINPUT_GAMEPAD_B)
						{

						}

						if (state.Gamepad.wButtons & XINPUT_GAMEPAD_X)
						{

						}

						if (state.Gamepad.wButtons & XINPUT_GAMEPAD_Y)
						{

						}
					}
					else
					{
						// TODO Error Handle
					}
				}

				DWORD dwCurrentPlayCursor = 0;
				DWORD dwCurrentWriteCursor = 0;

				g_pSoundBuffer->GetCurrentPosition(&dwCurrentPlayCursor, &dwCurrentWriteCursor);

				static DWORD dwWritePos = 0;

				//dwCurrentWriteCursor = dwWritePos;

				DWORD bytesToWrite = dwCurrentWriteCursor > dwCurrentPlayCursor
					? ((soundBufferSize - dwCurrentWriteCursor) + dwCurrentPlayCursor)
					: (dwCurrentPlayCursor - dwCurrentWriteCursor);

				void *pAudioLock1, *pAudioLock2;
				DWORD dwWriteSize1, dwWriteSize2;

				const DWORD feqInterval = (DWORD)(samplesPerSecond / note);
				static DWORD sampleCount = 0;
				HRESULT res = g_pSoundBuffer->Lock(dwCurrentWriteCursor, bytesToWrite, &pAudioLock1, &dwWriteSize1, &pAudioLock2, &dwWriteSize2, 0);
				if (SUCCEEDED(res))
				{
					dwWritePos = (dwWritePos + bytesToWrite) % soundBufferSize;
					uint16_t *pWritePoint1 = (uint16_t*)pAudioLock1;
					uint16_t *pWritePoint2 = (uint16_t*)pAudioLock2;

					sampleCount = Win32WriteSquareWaveToBuffer(pWritePoint1, dwWriteSize1, sampleCount, feqInterval, 600);
					sampleCount = Win32WriteSquareWaveToBuffer(pWritePoint2, dwWriteSize2, sampleCount, feqInterval, 600);

					HRESULT r = g_pSoundBuffer->Unlock(pAudioLock1, dwWriteSize1, pAudioLock2, dwWriteSize2);
					if (SUCCEEDED(r))
					{

					}
					else
					{
						OutputDebugStringA("ERROR: Unlocking the buffer failed\n");
					}
				}
				else
				{
					OutputDebugStringA("ERROR: Locking the buffer failed\n");
				}

				RenderTestGradient(&g_buffer, xOffset++, yOffset--);

				win32_window_size wndSize = GetWindowSize(hwnd);

				Win32CopyBufferToWindow(
					&g_buffer,
					dc,
					wndSize.width,
					wndSize.height,
					0,
					0,
					wndSize.width,
					wndSize.height);
			}

			ReleaseDC(hwnd, dc);
		}
		else
		{
			// TODO Handle Error
		}
	}
	else
	{
		// TODO Handle Error
	}


	return 0;
}

#include "Main.h"
#include "GameManager.h"

#include "Mouse.h"

#include "ImGUI/imgui.h"


#define APP_NAME "DX12"
#define CLASS_NAME "DX12"




HINSTANCE   g_Instance;
HWND        g_Window;
bool        g_FullWindow;
int         g_WindowWidth;
int         g_WindowHeight;



HWND GetWindow()
{
    return g_Window;
}



LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);





int APIENTRY wWinMain(  _In_ HINSTANCE hInstance,
                        _In_opt_ HINSTANCE hPrevInstance,
                        _In_ LPWSTR    lpCmdLine,
                        _In_ int       nCmdShow)
{

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);


    g_Instance = hInstance;
    g_FullWindow = false;




    int windowWidth, windowHeight;

    windowWidth = 1920;
    windowHeight = 1080;




    {
        WNDCLASSEX wcex;
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = 0;
        wcex.lpfnWndProc = WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = g_Instance;
        wcex.hIcon = nullptr;
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = nullptr;
        wcex.lpszMenuName = nullptr;
        wcex.lpszClassName = CLASS_NAME;
        wcex.hIconSm = nullptr;

        RegisterClassEx(&wcex);


        RECT rc = { 0, 0, (LONG)windowWidth, (LONG)windowHeight };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        g_WindowWidth = rc.right - rc.left;
        g_WindowHeight = rc.bottom - rc.top;

        g_Window = CreateWindow(CLASS_NAME, APP_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, g_Instance, nullptr);
    }





    {
        GameManager gameManager(g_Window);


        ShowWindow(g_Window, SW_SHOW);

        gameManager.Begin();

        //フレームカウント初期化
        DWORD dwExecLastTime;
        DWORD dwCurrentTime;
        timeBeginPeriod(1);
        dwExecLastTime = timeGetTime();
        dwCurrentTime = 0;

        while (true)
        {

            //DWORD frame = 0;
            MSG msg;

            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    break;
                }
                else
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
            else
            {
                dwCurrentTime = timeGetTime();

                if ((dwCurrentTime - dwExecLastTime) >= (1000 / 60 / 5))
                {
                    gameManager.Update();
                    gameManager.Draw();

                    dwExecLastTime = dwCurrentTime;
                }
                else
                {
                    Sleep(0);
                }


            }
        }
    }



    return 0;
}


void ChangeFullWindow()
{
    g_FullWindow = !g_FullWindow;

    if (g_FullWindow)
    {
        SetWindowLong(g_Window, GWL_STYLE, WS_POPUP);
        ShowWindow(g_Window, SW_MAXIMIZE);
    }
    else
    {
        SetWindowLong(g_Window, GWL_STYLE, WS_OVERLAPPEDWINDOW);

        SetWindowPos(g_Window, NULL, 0, 0, g_WindowWidth, g_WindowHeight, (SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED));
        ShowWindow(g_Window, SW_NORMAL);
    }
}



LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
        return true;

    switch (message)
    {

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    case WM_ACTIVATEAPP:
        Mouse_ProcessMessage(message, wParam, lParam);
        break;

    case WM_KEYDOWN:
        switch (wParam)
        {
        case VK_ESCAPE:
            DestroyWindow(hWnd);
            break;
        case VK_F11:
            ChangeFullWindow();
            break;
        }
        if (wParam == VK_ESCAPE) {
            SendMessage(hWnd, WM_CLOSE, 0, 0); // WM_CLOSEメッセージの送信
        }
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        //Keyboard_ProcessMessage(uMsg, wParam, lParam);
        break;
    case WM_INPUT:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEHOVER:
        Mouse_ProcessMessage(message, wParam, lParam);
        break;


    default:
        return DefWindowProc(hWnd, message, wParam, lParam);

    }

    return 0;
}

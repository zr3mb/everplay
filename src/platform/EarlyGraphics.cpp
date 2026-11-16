#include "EarlyGraphics.h"

#include <QColor>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <atomic>
#include <thread>

#include <d3d11_1.h>
#include <dxgi1_3.h>

namespace {

std::thread maker;
std::atomic<bool> made = false;
ID3D11Device *device = nullptr;
ID3D11DeviceContext *context = nullptr;

void drop()
{
    if (context)
        context->Release();
    if (device)
        device->Release();
    context = nullptr;
    device = nullptr;
}

// Only when Qt would make the same device itself: D3D11 on the first adapter, no debug layer, no software
// rasterizer — a variable that asks for something else leaves it all to Qt.
bool sameAsQt()
{
    const QByteArray backend = qgetenv("QSG_RHI_BACKEND");
    return (backend.isEmpty() || backend == "d3d11")
        && !qEnvironmentVariableIsSet("QT_QUICK_BACKEND")
        && !qEnvironmentVariableIsSet("QT_D3D_ADAPTER_INDEX")
        && !qEnvironmentVariableIsSet("QSG_RHI_PREFER_SOFTWARE_RENDERER")
        && !qEnvironmentVariableIsSet("QSG_RHI_DEBUG_LAYER");
}

// What QRhiD3D11::create() does for its own device: the first adapter, the default feature levels, and the
// checks it makes before using one (a device that fails them is left to Qt, which then falls back).
void makeDevice()
{
    IDXGIFactory2 *factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory))))
        return;
    IDXGIAdapter1 *adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     &device, nullptr, &context))) {
            device = nullptr;
            context = nullptr;
        }
        adapter->Release();
    }
    factory->Release();
    if (!device)
        return;
    ID3D11DeviceContext1 *context1 = nullptr;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
    const bool usable = SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&context1)))
        && SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)))
        && options.ConstantBufferOffsetting;
    if (context1)
        context1->Release();
    if (!usable)
        drop();
}

} // namespace

void EarlyGraphics::start()
{
    if (sameAsQt())
        maker = std::thread([] {
            makeDevice();
            made = true;
        });
}

void EarlyGraphics::prepare(QQuickWindow *window, const std::function<void()> &meanwhile)
{
    // Its background colour at once, through GDI: drawn into the window's own surface, which shows until the
    // swap chain presents its first frame (and is never seen again after that).
    if (window && window->handle()) {
        const HWND hwnd = reinterpret_cast<HWND>(window->winId());
        const QColor color = window->color();
        if (const HDC dc = GetDC(hwnd)) {
            RECT area;
            GetClientRect(hwnd, &area);
            const HBRUSH brush = CreateSolidBrush(RGB(color.red(), color.green(), color.blue()));
            FillRect(dc, &area, brush);
            DeleteObject(brush);
            ReleaseDC(hwnd, dc);
        }
    }

    if (!maker.joinable())
        return;
    if (!made && meanwhile)
        meanwhile();
    maker.join();
    if (!device)
        return;
    if (!window || window->isSceneGraphInitialized() || QQuickWindow::graphicsApi() != QSGRendererInterface::Direct3D11) {
        drop();
        return;
    }
    window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndContext(device, context));
    // Released graphics (a GPU reset, where this device is the lost one) are made anew from what the window holds:
    // from then on it's Qt's own device, as it would have been. This one stays referenced until the app ends —
    // the scene graph uses it without holding a reference of its own.
    QObject::connect(window, &QQuickWindow::sceneGraphInvalidated, window, [window] {
        window->setGraphicsDevice(QQuickGraphicsDevice());
    }, Qt::DirectConnection);   // on the render thread, before it makes the next device
}

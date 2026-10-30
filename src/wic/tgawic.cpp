// TgaWic.dll: Windows Imaging Component decoder for TGA (Truevision TARGA) images.
// Gives Explorer thumbnails, the preview pane and the Windows photo viewers through WIC.
// Install: regsvr32 TgaWic.dll (elevated). Uninstall: regsvr32 /u TgaWic.dll.
#include <windows.h>
#include <olectl.h>
#include <shlobj.h>
#include <wincodec.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "tga.h"

namespace {

// {6D74D57C-1B7B-4A5F-8C52-A1D245E4DC1D}
const CLSID CLSID_TgaDecoder = {0x6d74d57c, 0x1b7b, 0x4a5f, {0x8c, 0x52, 0xa1, 0xd2, 0x45, 0xe4, 0xdc, 0x1d}};
// {E816D0A3-820E-48BF-A73F-8EC03ED77CAF}
const GUID GUID_ContainerFormatTga = {0xe816d0a3, 0x820e, 0x48bf, {0xa7, 0x3f, 0x8e, 0xc0, 0x3e, 0xd7, 0x7c, 0xaf}};
// {33BF048D-34D4-4B06-8843-E4555FB220F4}
const GUID GUID_VendorTgaViewer = {0x33bf048d, 0x34d4, 0x4b06, {0x88, 0x43, 0xe4, 0x55, 0x5f, 0xb2, 0x20, 0xf4}};

const wchar_t kFriendlyName[] = L"TGA Decoder (Truevision TARGA)";
const wchar_t* const kExtensions[] = {L".tga", L".tpic", L".vda", L".icb", L".vst"};
const wchar_t kExtensionList[] = L".tga,.tpic,.vda,.icb,.vst";

// Shell handlers that render any WIC-decodable image.
const wchar_t kThumbnailHandlerKey[] = L"ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
const wchar_t kPhotoThumbnailProvider[] = L"{C7657C4A-9F68-40fa-A4DF-96BC08EB3551}";
const wchar_t kPreviewHandlerKey[] = L"ShellEx\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
const wchar_t kPhotoPreviewHandler[] = L"{FFE2A43C-56B9-4bf5-9A79-CC6D4285608A}";

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

// Minimal IUnknown implementation shared by the COM objects below.
template <class Derived>
class ComBase {
public:
    ComBase() { ++g_objects; }
    virtual ~ComBase() { --g_objects; }

protected:
    ULONG addRef() { return ++refs_; }
    ULONG release() {
        ULONG r = --refs_;
        if (r == 0) delete static_cast<Derived*>(this);
        return r;
    }

private:
    std::atomic<ULONG> refs_{1};
};

// Loaded file shared by the decoder and its frame. Pixels are decoded on first use.
struct Texture {
    std::vector<uint8_t> data;
    tga::Info info;
    std::mutex lock;
    tga::Image image;
    bool decoded = false;
    bool failed = false;
    std::atomic<ULONG> refs{1};

    void addRef() { ++refs; }
    void release() {
        if (--refs == 0) delete this;
    }

    const tga::Image* pixels() {
        std::lock_guard<std::mutex> guard(lock);
        if (!decoded && !failed) {
            failed = !tga::decode(data.data(), data.size(), info, image);
            decoded = !failed;
            if (decoded) {
                data.clear();
                data.shrink_to_fit();
            }
        }
        return decoded ? &image : nullptr;
    }
};

HRESULT readStream(IStream* stream, std::vector<uint8_t>& out) {
    if (!stream) return E_INVALIDARG;
    STATSTG st = {};
    HRESULT hr = stream->Stat(&st, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    if (st.cbSize.QuadPart < 18 || st.cbSize.QuadPart > (1ULL << 31)) return WINCODEC_ERR_BADIMAGE;
    LARGE_INTEGER zero = {};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return hr;
    out.resize(size_t(st.cbSize.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        ULONG got = 0;
        hr = stream->Read(out.data() + done, ULONG(out.size() - done), &got);
        if (FAILED(hr)) return hr;
        if (got == 0) break;
        done += got;
    }
    return done == out.size() ? S_OK : WINCODEC_ERR_BADIMAGE;
}

HRESULT copyRect(const tga::Image& img, const WICRect* prc, UINT stride, UINT bufferSize, BYTE* buffer) {
    WICRect full = {0, 0, INT(img.width), INT(img.height)};
    const WICRect& r = prc ? *prc : full;
    if (r.X < 0 || r.Y < 0 || r.Width < 0 || r.Height < 0 || UINT(r.X + r.Width) > img.width ||
        UINT(r.Y + r.Height) > img.height)
        return E_INVALIDARG;
    const UINT rowBytes = UINT(r.Width) * 4;
    if (stride < rowBytes) return E_INVALIDARG;
    if (r.Height > 0 && UINT64(stride) * (r.Height - 1) + rowBytes > bufferSize) return WINCODEC_ERR_INSUFFICIENTBUFFER;
    for (INT y = 0; y < r.Height; ++y)
        std::memcpy(buffer + size_t(stride) * y, &img.bgra[(size_t(r.Y + y) * img.width + r.X) * 4], rowBytes);
    return S_OK;
}

class Frame : public ComBase<Frame>, public IWICBitmapFrameDecode {
public:
    explicit Frame(Texture* tex) : tex_(tex) { tex_->addRef(); }
    ~Frame() override { tex_->release(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapSource || iid == IID_IWICBitmapFrameDecode) {
            *out = static_cast<IWICBitmapFrameDecode*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return addRef(); }
    STDMETHODIMP_(ULONG) Release() override { return release(); }

    // IWICBitmapSource
    STDMETHODIMP GetSize(UINT* w, UINT* h) override {
        if (!w || !h) return E_INVALIDARG;
        *w = tex_->info.width;
        *h = tex_->info.height;
        return S_OK;
    }
    STDMETHODIMP GetPixelFormat(WICPixelFormatGUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_WICPixelFormat32bppBGRA;
        return S_OK;
    }
    STDMETHODIMP GetResolution(double* x, double* y) override {
        if (!x || !y) return E_INVALIDARG;
        *x = *y = 96.0;
        return S_OK;
    }
    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    STDMETHODIMP CopyPixels(const WICRect* prc, UINT stride, UINT size, BYTE* buffer) override {
        if (!buffer) return E_INVALIDARG;
        const tga::Image* img = tex_->pixels();
        return img ? copyRect(*img, prc, stride, size, buffer) : WINCODEC_ERR_BADIMAGE;
    }

    // IWICBitmapFrameDecode
    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader**) override {
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource**) override { return WINCODEC_ERR_CODECNOTHUMBNAIL; }

private:
    Texture* tex_;
};

class Decoder : public ComBase<Decoder>, public IWICBitmapDecoder {
public:
    ~Decoder() override {
        if (tex_) tex_->release();
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapDecoder) {
            *out = static_cast<IWICBitmapDecoder*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return addRef(); }
    STDMETHODIMP_(ULONG) Release() override { return release(); }

    // IWICBitmapDecoder
    STDMETHODIMP QueryCapability(IStream* stream, DWORD* caps) override {
        if (!caps) return E_INVALIDARG;
        *caps = 0;
        std::vector<uint8_t> data;
        HRESULT hr = readStream(stream, data);
        if (FAILED(hr)) return hr;
        tga::Info info;
        // TGA has no signature: only a fully consistent header counts as a match.
        if (tga::parse(data.data(), data.size(), info))
            *caps = WICBitmapDecoderCapabilityCanDecodeAllImages | WICBitmapDecoderCapabilityCanDecodeSomeImages;
        return S_OK;
    }
    STDMETHODIMP Initialize(IStream* stream, WICDecodeOptions) override {
        if (tex_) return WINCODEC_ERR_WRONGSTATE;
        Texture* tex = new (std::nothrow) Texture;
        if (!tex) return E_OUTOFMEMORY;
        HRESULT hr = readStream(stream, tex->data);
        if (SUCCEEDED(hr) && !tga::parse(tex->data.data(), tex->data.size(), tex->info)) hr = WINCODEC_ERR_BADHEADER;
        if (FAILED(hr)) {
            tex->release();
            return hr;
        }
        tex_ = tex;
        return S_OK;
    }
    STDMETHODIMP GetContainerFormat(GUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_ContainerFormatTga;
        return S_OK;
    }
    STDMETHODIMP GetDecoderInfo(IWICBitmapDecoderInfo** info) override {
        if (!info) return E_INVALIDARG;
        *info = nullptr;
        IWICImagingFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        IWICComponentInfo* component = nullptr;
        hr = factory->CreateComponentInfo(CLSID_TgaDecoder, &component);
        if (SUCCEEDED(hr)) {
            hr = component->QueryInterface(IID_PPV_ARGS(info));
            component->Release();
        }
        factory->Release();
        return hr;
    }
    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader**) override {
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetPreview(IWICBitmapSource**) override { return WINCODEC_ERR_UNSUPPORTEDOPERATION; }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource**) override { return WINCODEC_ERR_CODECNOTHUMBNAIL; }
    STDMETHODIMP GetFrameCount(UINT* count) override {
        if (!count) return E_INVALIDARG;
        *count = tex_ ? 1 : 0;
        return tex_ ? S_OK : WINCODEC_ERR_NOTINITIALIZED;
    }
    STDMETHODIMP GetFrame(UINT index, IWICBitmapFrameDecode** frame) override {
        if (!frame) return E_INVALIDARG;
        *frame = nullptr;
        if (!tex_) return WINCODEC_ERR_NOTINITIALIZED;
        if (index != 0) return WINCODEC_ERR_FRAMEMISSING;
        Frame* f = new (std::nothrow) Frame(tex_);
        if (!f) return E_OUTOFMEMORY;
        *frame = f;
        return S_OK;
    }

private:
    Texture* tex_ = nullptr;
};

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *out = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }  // static instance
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Decoder* d = new (std::nothrow) Decoder;
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(iid, out);
        d->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks;
        else --g_locks;
        return S_OK;
    }
};

ClassFactory g_factory;

// ---- Registration -------------------------------------------------------------------------

std::wstring guidString(const GUID& g) {
    wchar_t buf[40];
    StringFromGUID2(g, buf, 40);
    return buf;
}

bool setValue(const std::wstring& path, const wchar_t* name, DWORD type, const void* data, DWORD size) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(data), size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool setString(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    return setValue(path, name, REG_SZ, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t)));
}

bool setDword(const std::wstring& path, const wchar_t* name, DWORD value) {
    return setValue(path, name, REG_DWORD, &value, sizeof(value));
}

const wchar_t kClasses[] = L"SOFTWARE\\Classes\\";
const wchar_t kKindMap[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap";

HRESULT registerServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return SELFREG_E_CLASS;

    const std::wstring clsid = guidString(CLSID_TgaDecoder);
    const std::wstring cls = std::wstring(kClasses) + L"CLSID\\" + clsid;
    bool ok = true;

    ok &= setString(cls, nullptr, kFriendlyName);
    ok &= setString(cls, L"FriendlyName", kFriendlyName);
    ok &= setString(cls, L"Author", L"tga-viewer");
    ok &= setString(cls, L"Version", L"1.0.0.0");
    ok &= setString(cls, L"SpecVersion", L"1.0.0.0");
    ok &= setString(cls, L"Vendor", guidString(GUID_VendorTgaViewer));
    ok &= setString(cls, L"ContainerFormat", guidString(GUID_ContainerFormatTga));
    ok &= setString(cls, L"FileExtensions", kExtensionList);
    ok &= setString(cls, L"MimeTypes", L"image/x-tga,image/x-targa");
    ok &= setDword(cls, L"SupportAnimation", 0);
    ok &= setDword(cls, L"SupportChromakey", 0);
    ok &= setDword(cls, L"SupportLossless", 1);
    ok &= setDword(cls, L"SupportMultiframe", 0);
    ok &= setDword(cls, L"ArbitrationPriority", 10);
    ok &= setString(cls + L"\\InprocServer32", nullptr, path);
    ok &= setString(cls + L"\\InprocServer32", L"ThreadingModel", L"Both");
    ok &= setString(cls + L"\\Formats\\" + guidString(GUID_WICPixelFormat32bppBGRA), nullptr, L"");

    // Content sniffing. TGA has no signature, so the pattern only checks what every TGA header
    // allows: color map type 0 or 1 (byte 1) and image type 1-3 or 9-11 (byte 2, masked 0xF4).
    // QueryCapability then validates the whole header.
    static const BYTE pattern[3] = {0x00, 0x00, 0x00};
    static const BYTE mask[3] = {0x00, 0xFE, 0xF4};
    const std::wstring pat = cls + L"\\Patterns\\0";
    ok &= setDword(pat, L"Position", 0);
    ok &= setDword(pat, L"Length", 3);
    ok &= setValue(pat, L"Pattern", REG_BINARY, pattern, 3);
    ok &= setValue(pat, L"Mask", REG_BINARY, mask, 3);

    // WIC decoder category.
    const std::wstring inst =
        std::wstring(kClasses) + L"CLSID\\" + guidString(CATID_WICBitmapDecoders) + L"\\Instance\\" + clsid;
    ok &= setString(inst, L"CLSID", clsid);
    ok &= setString(inst, L"FriendlyName", kFriendlyName);

    // Shell: picture kind plus the system photo thumbnail and preview handlers, which use WIC.
    for (const wchar_t* ext : kExtensions) {
        const std::wstring sfa = std::wstring(kClasses) + L"SystemFileAssociations\\" + ext;
        ok &= setString(sfa + L"\\" + kThumbnailHandlerKey, nullptr, kPhotoThumbnailProvider);
        ok &= setString(sfa + L"\\" + kPreviewHandlerKey, nullptr, kPhotoPreviewHandler);
        ok &= setString(sfa, L"PerceivedType", L"image");
        ok &= setString(kKindMap, ext, L"picture");
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok ? S_OK : SELFREG_E_CLASS;
}

HRESULT unregisterServer() {
    const std::wstring clsid = guidString(CLSID_TgaDecoder);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + clsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + guidString(CATID_WICBitmapDecoders) +
                                        L"\\Instance\\" + clsid).c_str());
    for (const wchar_t* ext : kExtensions) {
        const std::wstring sfa = std::wstring(kClasses) + L"SystemFileAssociations\\" + ext;
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, (sfa + L"\\" + kThumbnailHandlerKey).c_str());
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, (sfa + L"\\" + kPreviewHandlerKey).c_str());
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, sfa.c_str(), L"PerceivedType");
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kKindMap, ext);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != CLSID_TgaDecoder) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(iid, out);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() { return registerServer(); }

STDAPI DllUnregisterServer() { return unregisterServer(); }

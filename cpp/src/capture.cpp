// Capture cards through DirectShow, see capture.h.
// Graph: capture filter (+ crossbar and tuner found upstream by ICaptureGraphBuilder2),
//   VBI pin  -> Sample Grabber (raw VBI, one field per sample, KS_VIDEO_FLAG_FIELD1/2) -> Null Renderer
//   video pin -> Sample Grabber (YUY2) -> Null Renderer
// Fields are paired into frames in the layout of the capture chips' .vbi files (field 1 lines,
// then field 2 lines), so the frames can be written as a recording and read by the decoder.
#include "capture.h"
#include "legacy_bridge.h"
#include "ui/util.h"
#include <algorithm>
#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

// Sample Grabber (qedit.dll): qedit.h is gone from newer Windows SDKs, so the two interfaces are declared here
// (outside the anonymous namespace: there GCC sees no implementation of them and drops the calls)
static const GUID kSampleGrabber = {0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const GUID kIID_ISampleGrabber = {0x6B652FFF, 0x11FE, 0x4FCE, {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
static const GUID kIID_ISampleGrabberCB = {0x0579154A, 0x2B53, 0x4994, {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};
struct IGrabberCB : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double time, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double time, BYTE* buf, long len) = 0;
};
struct IGrabber : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(IGrabberCB*, long) = 0;
};

namespace {
// GUIDs from ksmedia.h / qedit.h that MinGW's libraries do not define
const GUID kRaw8 = {0xCA20D9A0, 0x3E3E, 0x11D1, {0x9B, 0xF9, 0x00, 0xC0, 0x4F, 0xBB, 0xDE, 0xBF}};
const GUID kSpecVbi = {0xF72A76E0, 0xEB0A, 0x11D0, {0xAC, 0xE4, 0x00, 0x00, 0xC0, 0xCC, 0x16, 0xBA}};
const GUID kNullRenderer = {0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

// KS_VIDEO_FLAG_* (ksmedia.h) in dwTypeSpecificFlags of a VBI sample
const DWORD kFieldMask = 3, kField1 = 1, kField2 = 2;
const long k525 = AnalogVideo_NTSC_M | AnalogVideo_NTSC_M_J | AnalogVideo_NTSC_433 | AnalogVideo_PAL_M | AnalogVideo_PAL_60;
const long k625 = AnalogVideo_PAL_B | AnalogVideo_PAL_D | AnalogVideo_PAL_G | AnalogVideo_PAL_H | AnalogVideo_PAL_I | AnalogVideo_PAL_N |
                  AnalogVideo_PAL_N_COMBO | AnalogVideo_SECAM_B | AnalogVideo_SECAM_D | AnalogVideo_SECAM_G | AnalogVideo_SECAM_H |
                  AnalogVideo_SECAM_K | AnalogVideo_SECAM_K1 | AnalogVideo_SECAM_L | AnalogVideo_SECAM_L1;

// a VBI type for 525 lines: the bt8x8 WDM driver marks its 625-line type (lines 7-22, 35.47 MHz) with
// every standard the chip knows, PAL M and NTSC 4.43 too, so the 625 bits and the first line decide
static bool vbi525(const KS_VBIINFOHEADER* h) { return h->StartLine >= 10 || (h->VideoStandard & k625) == 0; }

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::string narrowW(const wchar_t* w) { return w ? narrow(std::wstring(w)) : std::string(); }

void freeType(AM_MEDIA_TYPE& mt)
{
    if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
    mt.cbFormat = 0; mt.pbFormat = nullptr; mt.pUnk = nullptr;
}

GUID pinCategory(IPin* p)
{
    GUID c = GUID_NULL; DWORD r = 0;
    IKsPropertySet* ks = nullptr;
    if (SUCCEEDED(p->QueryInterface(IID_IKsPropertySet, (void**)&ks))) {
        ks->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &c, sizeof c, &r);
        ks->Release();
    }
    return c;
}

IPin* outputPin(IBaseFilter* f, const GUID& cat)
{
    IEnumPins* ep = nullptr;
    if (FAILED(f->EnumPins(&ep))) return nullptr;
    IPin* p;
    while (ep->Next(1, &p, nullptr) == S_OK) {
        PIN_DIRECTION d; p->QueryDirection(&d);
        if (d == PINDIR_OUTPUT && pinCategory(p) == cat) { ep->Release(); return p; }
        p->Release();
    }
    ep->Release();
    return nullptr;
}

IPin* firstPin(IBaseFilter* f, PIN_DIRECTION want)
{
    IEnumPins* ep = nullptr;
    if (FAILED(f->EnumPins(&ep))) return nullptr;
    IPin* p;
    while (ep->Next(1, &p, nullptr) == S_OK) {
        PIN_DIRECTION d; p->QueryDirection(&d);
        if (d == want) { ep->Release(); return p; }
        p->Release();
    }
    ep->Release();
    return nullptr;
}

std::vector<IMoniker*> categoryMonikers(REFCLSID cat, std::vector<std::string>* names)
{
    std::vector<IMoniker*> out;
    ICreateDevEnum* de = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&de))) return out;
    IEnumMoniker* em = nullptr;
    if (de->CreateClassEnumerator(cat, &em, 0) == S_OK) {
        IMoniker* m;
        while (em->Next(1, &m, nullptr) == S_OK) {
            if (names) {
                std::string n;
                IPropertyBag* pb = nullptr;
                if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&pb))) {
                    VARIANT v; VariantInit(&v);
                    if (SUCCEEDED(pb->Read(L"FriendlyName", &v, nullptr))) n = narrowW(v.bstrVal);
                    VariantClear(&v); pb->Release();
                }
                names->push_back(n);
            }
            out.push_back(m);
        }
        em->Release();
    }
    de->Release();
    return out;
}

std::vector<IMoniker*> videoMonikers(std::vector<std::string>* names) { return categoryMonikers(CLSID_VideoInputDeviceCategory, names); }

// an output pin with an audio type (Behold TV H8: "Audio PCM", 48 kHz stereo)
bool hasAudioOut(IBaseFilter* f)
{
    bool found = false;
    IEnumPins* ep = nullptr;
    if (FAILED(f->EnumPins(&ep))) return false;
    IPin* p;
    while (!found && ep->Next(1, &p, nullptr) == S_OK) {
        PIN_DIRECTION d; p->QueryDirection(&d);
        IEnumMediaTypes* et = nullptr;
        if (d == PINDIR_OUTPUT && SUCCEEDED(p->EnumMediaTypes(&et))) {
            AM_MEDIA_TYPE* mt = nullptr;
            while (et->Next(1, &mt, nullptr) == S_OK) {
                if (mt->majortype == MEDIATYPE_Audio) found = true;
                freeType(*mt); CoTaskMemFree(mt);
            }
            et->Release();
        }
        p->Release();
    }
    ep->Release();
    return found;
}

struct Grab : IGrabberCB {
    std::function<void(IMediaSample*)> fn;
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override
    {
        if (r == IID_IUnknown || r == kIID_ISampleGrabberCB) { *p = this; return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample* s) override { if (fn) fn(s); return S_OK; }
};

const char* physName(long t)
{
    switch (t) {
    case PhysConn_Video_Tuner: return "Tuner";
    case PhysConn_Video_Composite: return "Composite";
    case PhysConn_Video_SVideo: return "S-Video";
    case PhysConn_Video_RGB: return "RGB";
    case PhysConn_Video_YRYBY: return "Component";
    case PhysConn_Video_SerialDigital: return "Serial digital";
    case PhysConn_Video_ParallelDigital: return "Parallel digital";
    case PhysConn_Video_SCSI: return "SCSI";
    case PhysConn_Video_AUX: return "AUX";
    case PhysConn_Video_1394: return "1394";
    case PhysConn_Video_USB: return "USB";
    default: return "Video input";
    }
}
}

std::vector<CaptureDeviceInfo> captureDevices()
{
    std::vector<CaptureDeviceInfo> out;
    std::vector<std::string> names;
    auto ms = videoMonikers(&names);
    for (size_t i = 0; i < ms.size(); i++) {
        CaptureDeviceInfo d; d.name = names[i];
        IBaseFilter* f = nullptr;
        if (SUCCEEDED(ms[i]->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&f))) {
            IPin* p = outputPin(f, PIN_CATEGORY_VBI);
            d.vbi = p != nullptr;
            release(p); f->Release();
        }
        ms[i]->Release();
        out.push_back(d);
    }
    return out;
}

std::vector<std::string> audioInputs()
{
    std::vector<std::string> names;
    for (auto* m : categoryMonikers(CLSID_AudioInputDeviceCategory, &names)) m->Release();
    return names;
}

const std::vector<CaptureStandard>& captureStandards()
{
    static const std::vector<CaptureStandard> s = {
        {AnalogVideo_PAL_B, "PAL B/G"}, {AnalogVideo_PAL_D, "PAL D/K"}, {AnalogVideo_PAL_I, "PAL I"},
        {AnalogVideo_SECAM_D, "SECAM D/K"}, {AnalogVideo_SECAM_B, "SECAM B/G"}, {AnalogVideo_SECAM_L, "SECAM L"},
        {AnalogVideo_NTSC_M, "NTSC M"}, {AnalogVideo_NTSC_M_J, "NTSC Japan"}, {AnalogVideo_PAL_M, "PAL M"},
        {AnalogVideo_PAL_N, "PAL N"}, {AnalogVideo_NTSC_433, "NTSC 4.43"}};
    return s;
}

struct Capture::Impl {
    Capture* self = nullptr;
    IGraphBuilder* gb = nullptr;
    ICaptureGraphBuilder2* cg = nullptr;
    IMediaControl* mc = nullptr;
    IBaseFilter* cap = nullptr;
    IAMCrossbar* xb = nullptr;
    IAMTVTuner* tuner = nullptr;
    IAMAnalogVideoDecoder* dec = nullptr;
    IBaseFilter *vgrab = nullptr, *vnull = nullptr, *pgrab = nullptr, *pnull = nullptr;
    Grab vcb, pcb;
    bool running = false, connected = false;
    VbiGeometry g;
    std::string info;
    int fieldLines = 0, stride = 0, srcBytes = 0;
    // empty lines stored above the card's first one, so its lines fit a known layout (AVerMedia SAA7134: 7-23 = 17
    // lines a field, stored as 6-23 = the 18-line 1440-sample layout cx23885-pal)
    int padTop = 0;
    // layout chosen by hand (Capture::setLayout): empty lines above the card's to reach its lines a field
    VbiFormat wantLay = VbiFormat::Unknown;
    int padLay = 0;
    bool bt8x8 = false;   // Bt848/Bt878 WDM driver: VBI padded to 2048, small picture
    std::vector<uint8_t> frame;
    bool have1 = false;
    int nextField = 1;
    std::atomic<int64_t> nFields{0}, nLost{0};
    int vw = 0, vh = 0;
    int wantW = 0, wantH = 0;    // picture size asked for (0 = automatic)
    int vbiFirst = 0, vbiLast = 0;   // VBI lines asked for (0 = the driver's own)
    std::vector<int> inputPins;   // crossbar input pin index per video input
    int outVideo = -1, outAudio = -1;
    // sound: the card's own audio pin goes to a renderer in this graph; a sound card input plays in a graph of its own
    std::string audioSrc;
    bool cardAud = false, mute = false;
    IBaseFilter* arend = nullptr;
    IBasicAudio* ba = nullptr;
    IGraphBuilder* agb = nullptr;
    IMediaControl* amc = nullptr;
    IBasicAudio* aba = nullptr;

    void applyMute()
    {
        for (IBasicAudio* b : {ba, aba}) if (b) b->put_Volume(mute ? -10000 : 0);
    }
    // the card's own sound into this graph (graph stopped)
    void renderCardAudio()
    {
        if (audioSrc != "card" || !cardAud || arend) return;
        CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&arend);
        if (!arend) return;
        gb->AddFilter(arend, L"Sound");
        // small buffers: the sound keeps up with the picture
        IAMBufferNegotiation* bn = nullptr;
        if (SUCCEEDED(cg->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, cap, IID_IAMBufferNegotiation, (void**)&bn))) {
            ALLOCATOR_PROPERTIES ap{}; ap.cBuffers = 8; ap.cbBuffer = 48000 * 4 / 25; ap.cbAlign = -1; ap.cbPrefix = -1;
            bn->SuggestAllocatorProperties(&ap); bn->Release();
        }
        if (FAILED(cg->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, cap, nullptr, arend))) { gb->RemoveFilter(arend); release(arend); return; }
        arend->QueryInterface(IID_IBasicAudio, (void**)&ba);
        applyMute();
    }
    void removeCardAudio()
    {
        release(ba);
        if (arend) { if (gb) gb->RemoveFilter(arend); release(arend); }
    }
    // a sound card input, in its own graph, while the capture runs
    void startDeviceAudio()
    {
        if (audioSrc.empty() || audioSrc == "card" || agb) return;
        std::vector<std::string> names;
        auto ms = categoryMonikers(CLSID_AudioInputDeviceCategory, &names);
        IBaseFilter* src = nullptr;
        for (size_t i = 0; i < ms.size(); i++) if (!src && names[i] == audioSrc) ms[i]->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&src);
        for (auto* m : ms) m->Release();
        if (!src) return;
        ICaptureGraphBuilder2* acg = nullptr;
        CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&agb);
        CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void**)&acg);
        bool ok = agb && acg;
        if (ok) {
            acg->SetFiltergraph(agb);
            agb->AddFilter(src, L"Sound input");
            IAMBufferNegotiation* bn = nullptr;
            if (SUCCEEDED(acg->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, src, IID_IAMBufferNegotiation, (void**)&bn))) {
                ALLOCATOR_PROPERTIES ap{}; ap.cBuffers = 8; ap.cbBuffer = 48000 * 4 / 25; ap.cbAlign = -1; ap.cbPrefix = -1;
                bn->SuggestAllocatorProperties(&ap); bn->Release();
            }
            ok = SUCCEEDED(acg->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, src, nullptr, nullptr));
        }
        release(acg); src->Release();
        if (ok) {
            agb->QueryInterface(IID_IMediaControl, (void**)&amc);
            agb->QueryInterface(IID_IBasicAudio, (void**)&aba);
            applyMute();
            ok = amc && SUCCEEDED(amc->Run());
        }
        if (!ok) stopDeviceAudio();
    }
    void stopDeviceAudio()
    {
        if (amc) amc->Stop();
        release(aba); release(amc); release(agb);
    }

    // a .vbi file instead of a card
    std::string filePath;
    VbiFormat fileFormat = VbiFormat::Unknown;
    std::thread fileThread;
    std::atomic<bool> fileRun{false};
    std::atomic<int64_t> nLoops{0};
    void stopFile()
    {
        fileRun = false;
        if (fileThread.joinable()) fileThread.join();
    }
    void playFile()
    {
        VbiFile vf;
        std::string err;
        if (!vf.open(filePath, err, fileFormat)) return;
        const VbiGeometry& fg = vf.geom();
        auto period = std::chrono::nanoseconds((int64_t)(1e9 / (fg.pal ? 25.0 : 29.97)));
        auto next = std::chrono::steady_clock::now();
        std::vector<uint8_t> buf;
        int64_t f = 0;
        size_t fb = (size_t)fg.frameBytes();
        while (fileRun) {
            if (f >= fg.frames || vf.read(f, 1, buf) != 1 || buf.size() < fb) {
                if (f == 0) break;   // nothing to read
                f = 0; nLoops++;
                continue;
            }
            f++;
            nFields += 2;
            if (self->onFrame) self->onFrame(buf.data());
            next += period;
            auto now = std::chrono::steady_clock::now();
            if (next > now) std::this_thread::sleep_for(next - now);
            else if (now - next > std::chrono::seconds(1)) next = now;   // behind: no catching up in a burst
        }
    }

    void releaseAll()
    {
        stopFile();
        filePath.clear();
        if (mc && running) mc->Stop();
        running = false; connected = false;
        stopDeviceAudio(); removeCardAudio();
        release(vgrab); release(vnull); release(pgrab); release(pnull);
        release(xb); release(tuner); release(dec); release(cap); release(mc); release(cg); release(gb);
        inputPins.clear(); outVideo = outAudio = -1;
    }

    void removeGrabbers()
    {
        for (IBaseFilter** f : {&vgrab, &vnull, &pgrab, &pnull})
            if (*f) { gb->RemoveFilter(*f); release(*f); }
        removeCardAudio();
        connected = false;
    }

    void onVbi(IMediaSample* s)
    {
        BYTE* p = nullptr;
        if (FAILED(s->GetPointer(&p)) || !p) return;
        long n = s->GetActualDataLength();
        DWORD flags = 0;
        IMediaSample2* s2 = nullptr;
        if (SUCCEEDED(s->QueryInterface(IID_IMediaSample2, (void**)&s2))) {
            AM_SAMPLE2_PROPERTIES pr{};
            if (SUCCEEDED(s2->GetProperties(sizeof pr, (BYTE*)&pr))) flags = pr.dwTypeSpecificFlags;
            s2->Release();
        }
        const int pad = padTop + padLay;
        int lb = g.lineBytes, fb = (fieldLines + pad) * lb, sb = std::min(srcBytes, lb);
        static int dbg = 0;
        if (getenv("TR_VBI_DEBUG") && dbg < 6) { dbg++; fprintf(stderr, "VBI sample: %ld bytes, flags %lx, want %d lines x %d\n", n, flags, fieldLines, stride); }
        if (fieldLines <= 0 || n < (long)fieldLines * stride) return;
        auto copyField = [&](const BYTE* src, int field) {
            uint8_t* dst = frame.data() + (size_t)(field - 1) * fb;
            for (int l = 0; l < pad; l++) memset(dst + (size_t)l * lb, src[0], lb);   // blanking level
            dst += (size_t)pad * lb;
            for (int l = 0; l < fieldLines; l++) {
                uint8_t* d = dst + (size_t)l * lb;
                memcpy(d, src + (size_t)l * stride, sb);
                if (sb < lb) memset(d + sb, d[sb - 1], lb - sb);   // the line end repeated up to 2048
            }
        };
        if (n >= 2L * fieldLines * stride) {   // a whole frame in one sample
            copyField(p, 1); copyField(p + (size_t)fieldLines * stride, 2);
            if (self->onFrame) self->onFrame(frame.data());
            return;
        }
        int field = (flags & kFieldMask) == kField1 ? 1 : (flags & kFieldMask) == kField2 ? 2 : nextField;
        nextField = field == 1 ? 2 : 1;
        nFields++;
        copyField(p, field);
        if (field == 1) { if (have1) nLost++; have1 = true; }
        else if (have1) {
            have1 = false;
            if (self->onFrame) self->onFrame(frame.data());
        } else nLost++;
    }

    void onPicture(IMediaSample* s)
    {
        BYTE* p = nullptr;
        if (FAILED(s->GetPointer(&p)) || !p || vw <= 0) return;
        if (s->GetActualDataLength() < (long)vw * vh * 2) return;
        if (self->onVideo) self->onVideo(p, vw, vh, vw * 2);
    }

    bool connect(std::string& err)
    {
        if (connected) return true;
        removeGrabbers();
        IPin* vp = outputPin(cap, PIN_CATEGORY_VBI);
        if (!vp) { err = "the card has no raw VBI output (VBI pin)"; return false; }
        // the VBI type of the current standard (525 or 625 lines)
        long std_ = 0;
        if (dec) dec->get_TVFormat(&std_);
        bool want525 = (std_ & k525) != 0;
        AM_MEDIA_TYPE chosen{}; bool have = false;
        IEnumMediaTypes* et = nullptr;
        if (SUCCEEDED(vp->EnumMediaTypes(&et))) {
            AM_MEDIA_TYPE* mt;
            while (et->Next(1, &mt, nullptr) == S_OK) {
                bool ok = mt->majortype == MEDIATYPE_VBI && mt->subtype == kRaw8 && mt->formattype == kSpecVbi && mt->cbFormat >= sizeof(KS_VBIINFOHEADER);
                if (ok && getenv("TR_VBI_DEBUG")) {
                    auto* h = (KS_VBIINFOHEADER*)mt->pbFormat;
                    fprintf(stderr, "VBI type: lines %lu-%lu, %lu Hz, %lu samples, stride %lu, start %ld..%ld us x100, std %lx\n", h->StartLine,
                            h->EndLine, h->SamplingFrequency, h->SamplesPerLine, h->StrideInBytes, h->MinLineStartTime, h->MaxLineStartTime,
                            h->VideoStandard);
                }
                if (ok) {
                    auto* h = (KS_VBIINFOHEADER*)mt->pbFormat;
                    bool is525 = vbi525(h);
                    if (!have || is525 == want525) {
                        if (have) freeType(chosen);
                        chosen = *mt; chosen.pbFormat = (BYTE*)CoTaskMemAlloc(mt->cbFormat); memcpy(chosen.pbFormat, mt->pbFormat, mt->cbFormat);
                        if (chosen.pUnk) chosen.pUnk->AddRef();
                        have = true;
                        if (is525 == want525) { freeType(*mt); CoTaskMemFree(mt); break; }
                    }
                }
                freeType(*mt); CoTaskMemFree(mt);
            }
            et->Release();
        }
        if (!have) { vp->Release(); err = "the VBI pin offers no raw 8-bit VBI format"; return false; }
        // a second of fields in buffers: with the few the driver takes by default it drops fields
        // whenever the processor is busy for a moment
        IAMBufferNegotiation* bn = nullptr;
        if (SUCCEEDED(vp->QueryInterface(IID_IAMBufferNegotiation, (void**)&bn))) {
            ALLOCATOR_PROPERTIES ap{};
            ap.cBuffers = 50; ap.cbBuffer = -1; ap.cbAlign = -1; ap.cbPrefix = -1;
            bn->SuggestAllocatorProperties(&ap);
            bn->Release();
        }
        CoCreateInstance(kSampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&vgrab);
        CoCreateInstance(kNullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&vnull);
        if (!vgrab || !vnull) { vp->Release(); freeType(chosen); err = "Sample Grabber (qedit.dll) is not available"; return false; }
        gb->AddFilter(vgrab, L"VBI grabber"); gb->AddFilter(vnull, L"VBI sink");
        IGrabber* sg = nullptr;
        vgrab->QueryInterface(kIID_ISampleGrabber, (void**)&sg);
        AM_MEDIA_TYPE want{}; want.majortype = MEDIATYPE_VBI; want.subtype = kRaw8;
        sg->SetMediaType(&want);
        vcb.fn = [this](IMediaSample* s) { onVbi(s); };
        sg->SetCallback(&vcb, 0);
        IPin* gin = firstPin(vgrab, PINDIR_INPUT);
        // another range of lines per field (data outside 7-22): setVbiLines(), or TR_VBI_LINES=a-b
        unsigned a = vbiFirst, b = vbiLast;
        if (!a) if (const char* e = getenv("TR_VBI_LINES")) sscanf(e, "%u-%u", &a, &b);
        bool otherLines = a && b >= a;
        if (otherLines) {
            auto* h = (KS_VBIINFOHEADER*)chosen.pbFormat;
            h->StartLine = a; h->EndLine = b;
            h->BufferSize = h->StrideInBytes * (b - a + 1) * 2;
            chosen.lSampleSize = h->BufferSize;
        }
        // through the Kernel Tee (Tee/Sink-to-Sink Converter) the capture graph builder puts after the pin:
        // the bt8x8 WDM driver (Conexant BtPCI) connects straight to the grabber too but then sends nothing
        HRESULT hr = E_FAIL;
        bool viaTee = !otherLines;
        if (viaTee) {
            IAMStreamConfig* sc = nullptr;
            if (SUCCEEDED(vp->QueryInterface(IID_IAMStreamConfig, (void**)&sc))) { sc->SetFormat(&chosen); sc->Release(); }
            hr = cg->RenderStream(&PIN_CATEGORY_VBI, &MEDIATYPE_VBI, cap, vgrab, vnull);
            if (FAILED(hr)) {   // undo what the builder left half connected, then straight to the grabber
                viaTee = false;
                IPin* other = nullptr;
                if (SUCCEEDED(vp->ConnectedTo(&other)) && other) { gb->Disconnect(other); gb->Disconnect(vp); other->Release(); }
            }
        }
        if (!viaTee) {
            hr = gb->ConnectDirect(vp, gin, &chosen);
            if (FAILED(hr)) hr = gb->Connect(vp, gin);
        }
        release(gin);
        if (SUCCEEDED(hr) && !viaTee) {
            IPin* gout = firstPin(vgrab, PINDIR_OUTPUT); IPin* nin = firstPin(vnull, PINDIR_INPUT);
            hr = gb->Connect(gout, nin);
            release(gout); release(nin);
        }
        AM_MEDIA_TYPE got{};
        if (SUCCEEDED(hr) && SUCCEEDED(sg->GetConnectedMediaType(&got)) && got.cbFormat >= sizeof(KS_VBIINFOHEADER)) {
            freeType(chosen); chosen = got;
        }
        sg->Release(); vp->Release();
        if (FAILED(hr)) { freeType(chosen); err = "could not connect the VBI pin"; return false; }
        auto* h = (KS_VBIINFOHEADER*)chosen.pbFormat;
        bool is525 = vbi525(h);
        fieldLines = (int)(h->EndLine - h->StartLine + 1);
        stride = (int)h->StrideInBytes;
        srcBytes = (int)h->SamplesPerLine;
        // bt8x8 (35.47 MHz PAL / 28.64 MHz NTSC, 2000 / 1600 samples): lines padded to 2048 like the Linux
        // bttv / cx88 files, so a recording opens as bttv-pal / cx88 in the full decoder
        bt8x8 = std::abs((double)h->SamplingFrequency - (is525 ? 28636363.0 : 35468950.0)) < 2000 && srcBytes <= 2048;
        VbiFormat lay = bt8x8 ? (is525 ? VbiFormat::Cx88 : VbiFormat::BttvPal) : is525 ? VbiFormat::Cx23885 : VbiFormat::Saa713xPal;
        // 18 lines a field asked for (Lines..., e.g. 5-22): the 18-line layouts of the same sampling, so a
        // recording opens as cx88-pal / cx23885-pal (the first line is written next to it in <recording>.lines)
        if (!is525 && fieldLines == 18 && (bt8x8 || srcBytes == 1440)) lay = bt8x8 ? VbiFormat::Cx88Pal : VbiFormat::Cx23885Pal;
        // 17 lines from line 7 (AVerMedia SAA7134 / SAA7130 PAL): one empty line above -> 6-23, the 18-line layout
        padTop = !is525 && !bt8x8 && srcBytes == 1440 && fieldLines == 17 && h->StartLine == 7 ? 1 : 0;
        if (padTop) lay = VbiFormat::Cx23885Pal;
        // a layout set by hand: the same bytes a line and at least the card's lines a field (padded above)
        padLay = 0;
        if (wantLay != VbiFormat::Unknown) {
            VbiGeometry fg = formatGeometry(wantLay);
            const int bytes = bt8x8 ? 2048 : srcBytes, lf = fg.linesPerFrame / 2;
            if (fg.lineBytes == bytes && lf >= fieldLines + padTop) { lay = wantLay; padLay = lf - fieldLines - padTop; }
        }
        g = formatGeometry(lay);
        g.validSamples = srcBytes;
        if (!bt8x8) g.samplesPerLine = g.lineBytes = srcBytes;
        g.linesPerFrame = 2 * (fieldLines + padTop + padLay);
        g.firstLine = (int)h->StartLine - padTop - padLay;
        // some drivers (AVerMedia SAA7134, Windows: 312) give the first VBI line in another numbering: the
        // layout's own first line then
        if ((!is525 && g.firstLine > 300) || (is525 && g.firstLine > 250)) g.firstLine = formatGeometry(lay).firstLine;
        g.storedPerField = 0; g.skipLines = 0;
        if (h->SamplingFrequency) g.lineSamples = (float)(h->SamplingFrequency / g.lineRate);
        frame.assign((size_t)g.frameBytes(), 0);
        have1 = false; nextField = 1;
        info = fmt_info(h);
        freeType(chosen);
        // the picture (optional: VBI works without it)
        CoCreateInstance(kSampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&pgrab);
        CoCreateInstance(kNullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&pnull);
        if (pgrab && pnull) {
            gb->AddFilter(pgrab, L"Video grabber"); gb->AddFilter(pnull, L"Video sink");
            IGrabber* pg = nullptr;
            pgrab->QueryInterface(kIID_ISampleGrabber, (void**)&pg);
            AM_MEDIA_TYPE vt{}; vt.majortype = MEDIATYPE_Video; vt.subtype = MEDIASUBTYPE_YUY2; vt.formattype = FORMAT_VideoInfo;
            pg->SetMediaType(&vt);
            if (getenv("TR_VBI_DEBUG")) {   // the picture formats the card offers
                IPin* cp = outputPin(cap, PIN_CATEGORY_CAPTURE);
                IAMStreamConfig* sc = nullptr;
                if (cp && SUCCEEDED(cp->QueryInterface(IID_IAMStreamConfig, (void**)&sc))) {
                    int cnt = 0, sz = 0;
                    sc->GetNumberOfCapabilities(&cnt, &sz);
                    std::vector<BYTE> caps(sz > 0 ? sz : 1);
                    for (int i = 0; i < cnt; i++) {
                        AM_MEDIA_TYPE* m = nullptr;
                        if (SUCCEEDED(sc->GetStreamCaps(i, &m, caps.data())) && m) {
                            auto* vc = (VIDEO_STREAM_CONFIG_CAPS*)caps.data();
                            fprintf(stderr, "video cap %d: %.4s  min %ldx%ld max %ldx%ld\n", i, (char*)&m->subtype.Data1, vc->MinOutputSize.cx,
                                    vc->MinOutputSize.cy, vc->MaxOutputSize.cx, vc->MaxOutputSize.cy);
                            freeType(*m); CoTaskMemFree(m);
                        }
                    }
                    sc->Release();
                }
                if (cp) cp->Release();
            }
            // bt8x8: with a picture larger than one field (384x576, 640x288 and up) the card loses VBI fields
            // (behind a PCIe-PCI bridge ~17 instead of 50 a second), so the picture is one field, 384x288
            // (a size chosen in the window wins)
            if (IPin* cp = bt8x8 || wantW > 0 ? outputPin(cap, PIN_CATEGORY_CAPTURE) : nullptr) {
                IAMStreamConfig* sc = nullptr;
                if (SUCCEEDED(cp->QueryInterface(IID_IAMStreamConfig, (void**)&sc))) {
                    long w = wantW > 0 ? wantW : is525 ? 320 : 384, hh = wantW > 0 ? wantH : is525 ? 240 : 288;
                    int cnt = 0, sz = 0;
                    sc->GetNumberOfCapabilities(&cnt, &sz);
                    std::vector<BYTE> caps(sz > 0 ? sz : 1);
                    for (int i = 0; i < cnt; i++) {
                        AM_MEDIA_TYPE* m = nullptr;
                        if (FAILED(sc->GetStreamCaps(i, &m, caps.data())) || !m) continue;
                        auto* vc = (VIDEO_STREAM_CONFIG_CAPS*)caps.data();
                        bool done = false;
                        if (m->subtype == MEDIASUBTYPE_YUY2 && m->formattype == FORMAT_VideoInfo && vc->MaxOutputSize.cx >= w &&
                            vc->MinOutputSize.cx <= w && vc->MaxOutputSize.cy >= hh && vc->MinOutputSize.cy <= hh) {
                            auto* vi = (VIDEOINFOHEADER*)m->pbFormat;
                            vi->bmiHeader.biWidth = w; vi->bmiHeader.biHeight = hh;
                            vi->bmiHeader.biSizeImage = (DWORD)(w * hh * 2);
                            m->lSampleSize = vi->bmiHeader.biSizeImage;
                            done = SUCCEEDED(sc->SetFormat(m));
                        }
                        freeType(*m); CoTaskMemFree(m);
                        if (done) break;
                    }
                    sc->Release();
                }
                cp->Release();
            }
            pcb.fn =[this](IMediaSample* s) { onPicture(s); };
            pg->SetCallback(&pcb, 0);
            vw = vh = 0;
            if (SUCCEEDED(cg->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, cap, pgrab, pnull))) {
                AM_MEDIA_TYPE vm{};
                if (SUCCEEDED(pg->GetConnectedMediaType(&vm))) {
                    if (vm.formattype == FORMAT_VideoInfo && vm.cbFormat >= sizeof(VIDEOINFOHEADER)) {
                        auto* vi = (VIDEOINFOHEADER*)vm.pbFormat;
                        vw = vi->bmiHeader.biWidth; vh = abs(vi->bmiHeader.biHeight);
                    }
                    freeType(vm);
                }
            }
            pg->Release();
        }
        renderCardAudio();
        connected = true;
        return true;
    }

    static std::string fmt_info(const KS_VBIINFOHEADER* h)
    {
        bool is525 = vbi525(h);
        int off = is525 ? 263 : 313;
        char b[160];
        snprintf(b, sizeof b, "lines %lu-%lu / %lu-%lu, %.2f MHz, %lu samples per line", h->StartLine, h->EndLine,
                 h->StartLine + off, h->EndLine + off, h->SamplingFrequency / 1e6, h->SamplesPerLine);
        return b;
    }
};

static std::string fmt_frames(int64_t n, bool pal)
{
    char b[64];
    double s = n / (pal ? 25.0 : 29.97);
    snprintf(b, sizeof b, ", %lld frames = %d:%02d, played over and over", (long long)n, (int)s / 60, (int)s % 60);
    return b;
}

Capture::Capture() : d(new Impl) { d->self = this; }
Capture::~Capture() { close(); }
bool Capture::isOpen() const { return d->cap != nullptr || !d->filePath.empty(); }
bool Capture::isFile() const { return !d->filePath.empty(); }
int64_t Capture::loops() const { return d->nLoops; }

bool Capture::openFile(const std::string& path, std::string& err, VbiFormat force)
{
    close();
    // the layout as the recording's decoder finds it: several chips share a file size (2с.vbi, SAA713x
    // PAL 1440 x 32, also fits CX23885 1440 x 24 with a part frame), and a wrong one rolls the lines
    VbiFormat fm = force;
    if (fm == VbiFormat::Unknown) {
        int64_t size = 0;
        if (FILE* f = _wfopen(widen(path).c_str(), L"rb")) { _fseeki64(f, 0, SEEK_END); size = _ftelli64(f); fclose(f); }
        auto cands = candidateFormats(size);
        if (cands.size() > 1) {
            VbiFormat lf = legacyDetectFormat(path);
            if (std::find(cands.begin(), cands.end(), lf) != cands.end()) fm = lf;
            else for (VbiFormat c : cands) if (size % formatGeometry(c).fileFrameBytes() == 0) { fm = c; break; }
        }
    }
    VbiFile vf;
    if (!vf.open(path, err, fm)) return false;
    d->fileFormat = vf.geom().format;
    if (vf.geom().frames <= 0) { err = "no whole frame in the file"; return false; }
    d->g = vf.geom();
    d->filePath = path;
    std::string name = path.substr(path.find_last_of("\\/") + 1);
    d->info = name + " (" + formatName(d->g.format) + fmt_frames(d->g.frames, d->g.pal) + ")";
    return true;
}
bool Capture::running() const { return d->running; }
const VbiGeometry& Capture::geom() const { return d->g; }
std::string Capture::vbiInfo() const { return d->info; }
int64_t Capture::fields() const { return d->nFields; }
int64_t Capture::lostFields() const { return d->nLost; }

bool Capture::open(int index, std::string& err)
{
    close();
    auto ms = videoMonikers(nullptr);
    if (index < 0 || index >= (int)ms.size()) { for (auto* m : ms) m->Release(); err = "no such capture device"; return false; }
    HRESULT hr = ms[index]->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&d->cap);
    for (auto* m : ms) m->Release();
    if (FAILED(hr) || !d->cap) { err = "the device cannot be opened (in use by another program?)"; return false; }
    d->cardAud = hasAudioOut(d->cap);
    CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&d->gb);
    CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void**)&d->cg);
    if (!d->gb || !d->cg) { d->releaseAll(); err = "DirectShow is not available"; return false; }
    d->cg->SetFiltergraph(d->gb);
    d->gb->AddFilter(d->cap, L"Capture");
    d->gb->QueryInterface(IID_IMediaControl, (void**)&d->mc);
    d->cg->FindInterface(&LOOK_UPSTREAM_ONLY, nullptr, d->cap, IID_IAMCrossbar, (void**)&d->xb);
    d->cg->FindInterface(&LOOK_UPSTREAM_ONLY, nullptr, d->cap, IID_IAMTVTuner, (void**)&d->tuner);
    d->cap->QueryInterface(IID_IAMAnalogVideoDecoder, (void**)&d->dec);
    if (d->xb) {
        long no = 0, ni = 0;
        d->xb->get_PinCounts(&no, &ni);
        for (long i = 0; i < ni; i++) {
            long rel, type;
            if (SUCCEEDED(d->xb->get_CrossbarPinInfo(TRUE, i, &rel, &type)) && type < PhysConn_Audio_Tuner) d->inputPins.push_back((int)i);
        }
        for (long o = 0; o < no; o++) {
            long rel, type;
            if (FAILED(d->xb->get_CrossbarPinInfo(FALSE, o, &rel, &type))) continue;
            if (type == PhysConn_Video_VideoDecoder && d->outVideo < 0) d->outVideo = (int)o;
            if (type == PhysConn_Audio_AudioDecoder && d->outAudio < 0) d->outAudio = (int)o;
        }
    }
    return true;
}

void Capture::close() { d->releaseAll(); }

std::vector<std::string> Capture::inputs() const
{
    std::vector<std::string> out;
    for (int i : d->inputPins) {
        long rel, type = 0;
        d->xb->get_CrossbarPinInfo(TRUE, i, &rel, &type);
        std::string n = physName(type);
        int same = 0;
        for (auto& o : out) if (o.rfind(n, 0) == 0) same++;
        out.push_back(same ? n + " " + std::to_string(same + 1) : n);
    }
    return out;
}

int Capture::input() const
{
    if (!d->xb || d->outVideo < 0) return -1;
    long r = -1;
    d->xb->get_IsRoutedTo(d->outVideo, &r);
    for (size_t k = 0; k < d->inputPins.size(); k++) if (d->inputPins[k] == r) return (int)k;
    return -1;
}

bool Capture::isTunerInput(int i) const
{
    if (!d->xb || i < 0 || i >= (int)d->inputPins.size()) return false;
    long rel, type = 0;
    d->xb->get_CrossbarPinInfo(TRUE, d->inputPins[i], &rel, &type);
    return type == PhysConn_Video_Tuner;
}

bool Capture::setInput(int i)
{
    if (!d->xb || d->outVideo < 0 || i < 0 || i >= (int)d->inputPins.size()) return false;
    int pin = d->inputPins[i];
    if (FAILED(d->xb->Route(d->outVideo, pin))) return false;
    // the sound of that input too
    long rel = -1, type;
    if (d->outAudio >= 0 && SUCCEEDED(d->xb->get_CrossbarPinInfo(TRUE, pin, &rel, &type)) && rel >= 0) d->xb->Route(d->outAudio, rel);
    return true;
}

long Capture::standard() const { long s = 0; if (d->dec) d->dec->get_TVFormat(&s); return s; }
long Capture::standards() const { long s = 0; if (d->dec) d->dec->get_AvailableTVFormats(&s); return s; }

bool Capture::setStandard(long bit)
{
    if (!d->dec) return false;
    bool was = d->running;
    if (was) stop();
    bool lines525 = (bit & k525) != 0, before525 = (standard() & k525) != 0;
    bool ok = SUCCEEDED(d->dec->put_TVFormat(bit));
    if (lines525 != before525) d->removeGrabbers();   // the VBI layout changes with the number of lines
    if (was) { std::string e; run(e); }
    return ok;
}

std::vector<std::pair<int, int>> Capture::pictureSizes() const
{
    std::vector<std::pair<int, int>> out;
    IPin* cp = d->cap ? outputPin(d->cap, PIN_CATEGORY_CAPTURE) : nullptr;
    if (!cp) return out;
    bool is525 = (standard() & k525) != 0;
    static const int pal[][2] = {{768, 576}, {720, 576}, {704, 576}, {640, 480}, {384, 576}, {384, 288}, {352, 288}, {320, 240}, {192, 144}};
    static const int ntsc[][2] = {{720, 480}, {640, 480}, {704, 480}, {352, 480}, {320, 240}, {352, 240}, {160, 120}};
    IAMStreamConfig* sc = nullptr;
    if (SUCCEEDED(cp->QueryInterface(IID_IAMStreamConfig, (void**)&sc))) {
        int cnt = 0, sz = 0;
        sc->GetNumberOfCapabilities(&cnt, &sz);
        std::vector<BYTE> caps(sz > 0 ? sz : 1);
        std::vector<VIDEO_STREAM_CONFIG_CAPS> yuy;
        for (int i = 0; i < cnt; i++) {
            AM_MEDIA_TYPE* m = nullptr;
            if (FAILED(sc->GetStreamCaps(i, &m, caps.data())) || !m) continue;
            if (m->subtype == MEDIASUBTYPE_YUY2 && m->formattype == FORMAT_VideoInfo) yuy.push_back(*(VIDEO_STREAM_CONFIG_CAPS*)caps.data());
            freeType(*m); CoTaskMemFree(m);
        }
        sc->Release();
        auto add = [&](int w, int h) {
            for (auto& vc : yuy)
                if (vc.MinOutputSize.cx <= w && vc.MaxOutputSize.cx >= w && vc.MinOutputSize.cy <= h && vc.MaxOutputSize.cy >= h) { out.push_back({w, h}); return; }
        };
        if (is525) for (auto& s : ntsc) add(s[0], s[1]);
        else for (auto& s : pal) add(s[0], s[1]);
    }
    cp->Release();
    return out;
}

void Capture::setLayout(VbiFormat f)
{
    if (f == d->wantLay) return;
    bool was = d->running;
    if (was) stop();
    d->wantLay = f;
    if (d->gb) d->removeGrabbers();
    if (was) { std::string e; run(e); }
}
VbiFormat Capture::layout() const { return d->g.format; }
VbiFormat Capture::wantedLayout() const { return d->wantLay; }

void Capture::setPictureSize(int w, int h)
{
    if (w == d->wantW && h == d->wantH) return;
    bool was = d->running;
    if (was) stop();
    d->wantW = w; d->wantH = h;
    if (d->gb) d->removeGrabbers();
    if (was) { std::string e; run(e); }
}

bool Capture::bt8x8() const { return d->bt8x8; }
bool Capture::cardAudio() const { return d->cardAud; }
const std::string& Capture::audio() const { return d->audioSrc; }
bool Capture::audioPlaying() const { return d->running && (d->ba || d->amc); }
bool Capture::muted() const { return d->mute; }
void Capture::setMute(bool m) { d->mute = m; d->applyMute(); }

void Capture::setAudio(const std::string& source)
{
    if (source == d->audioSrc) return;
    bool card = source == "card" || d->audioSrc == "card", was = d->running;
    if (card && was) stop();
    d->stopDeviceAudio();
    d->audioSrc = source;
    if (card) {
        if (d->gb) d->removeGrabbers();
        if (was) { std::string e; run(e); }
    } else if (d->running) d->startDeviceAudio();
}
std::pair<int, int> Capture::pictureSize() const { return {d->vw, d->vh}; }

void Capture::setVbiLines(int first, int last)
{
    if (first == d->vbiFirst && last == d->vbiLast) return;
    bool was = d->running;
    if (was) stop();
    d->vbiFirst = first; d->vbiLast = last;
    if (d->gb) d->removeGrabbers();
    if (was) { std::string e; run(e); }
}

std::pair<int, int> Capture::vbiLines() const
{
    if (!d->fieldLines) return {0, 0};
    return {d->g.firstLine, d->g.firstLine + d->fieldLines + d->padTop + d->padLay - 1};
}

bool Capture::hasTuner() const { return d->tuner != nullptr; }
long Capture::channel() const { long c = -1, v, a; if (d->tuner) d->tuner->get_Channel(&c, &v, &a); return c; }
bool Capture::setChannel(long ch) { return d->tuner && SUCCEEDED(d->tuner->put_Channel(ch, AMTUNER_SUBCHAN_DEFAULT, AMTUNER_SUBCHAN_DEFAULT)); }
bool Capture::locked() const { long l = 0; if (d->dec) d->dec->get_HorizontalLocked(&l); return l != 0; }

bool Capture::run(std::string& err)
{
    if (d->running) return true;
    if (!d->filePath.empty()) {
        d->nFields = 0; d->nLost = 0; d->nLoops = 0;
        d->fileRun = true;
        d->running = true;
        d->fileThread = std::thread([this] { d->playFile(); });
        return true;
    }
    if (!d->cap) { err = "no device"; return false; }
    if (!d->connect(err)) return false;
    // no reference clock: the renderers would drop samples they think late, the grabbers take every one.
    // The AVerMedia SAA7134/7130 driver (M151, its 17-line VBI layout) sends no VBI at all without a clock: it keeps
    // the graph's own (tools/aver/avpins: 0 VBI samples in 3 s without, 51 fields a second with)
    IMediaFilter* mf = nullptr;
    if (!d->padTop && SUCCEEDED(d->gb->QueryInterface(IID_IMediaFilter, (void**)&mf))) { mf->SetSyncSource(nullptr); mf->Release(); }
    // there the renderers alone go without a clock (else they hold every sample till its time and the grabbers wait)
    HRESULT hr0 = S_OK;
    if (d->padTop) {
        d->mc->Pause();
        for (IBaseFilter* f : {d->vnull, d->pnull})
            if (f) hr0 = f->SetSyncSource(nullptr);
        d->mc->Stop();
    }
    (void)hr0;
    HRESULT hr = d->mc->Run();
    if (FAILED(hr)) { err = "the capture graph does not start (the card may be in use by another program)"; return false; }
    d->running = true;
    d->startDeviceAudio();
    d->nFields = 0; d->nLost = 0;
    return true;
}

void Capture::stop()
{
    d->stopFile();
    d->stopDeviceAudio();
    if (d->mc && d->running) d->mc->Stop();
    d->running = false;
}

// Behold (Beholder International) driver properties: line / frame sync of the video decoder
static const GUID kBeholdProps = {0x2BC5F538, 0x3430, 0x44E0, {0x83, 0x08, 0x96, 0x7D, 0x1B, 0xF2, 0xD7, 0x72}};

int Capture::syncMode(int which) const
{
    if (!d->cap || which < 0 || which > 1) return -1;
    IKsPropertySet* ks = nullptr;
    if (FAILED(d->cap->QueryInterface(IID_IKsPropertySet, (void**)&ks)) || !ks) return -1;
    DWORD sup = 0, v = 0, got = 0;
    int out = -1;
    if (SUCCEEDED(ks->QuerySupported(kBeholdProps, 17 + which, &sup)) && (sup & KSPROPERTY_SUPPORT_GET) &&
        SUCCEEDED(ks->Get(kBeholdProps, 17 + which, nullptr, 0, &v, sizeof v, &got)) && got >= sizeof v && v < 4)
        out = (int)v;
    ks->Release();
    return out;
}

bool Capture::setSyncMode(int which, int mode)
{
    if (!d->cap || which < 0 || which > 1 || mode < 0 || mode > 3) return false;
    IKsPropertySet* ks = nullptr;
    if (FAILED(d->cap->QueryInterface(IID_IKsPropertySet, (void**)&ks)) || !ks) return false;
    DWORD v = (DWORD)mode;
    bool ok = SUCCEEDED(ks->Set(kBeholdProps, 17 + which, nullptr, 0, &v, sizeof v));
    ks->Release();
    return ok;
}

#pragma once
// Capture cards on Windows (DirectShow): the raw VBI pin (MEDIATYPE_VBI / RAW8 with a
// KS_VBIINFOHEADER, e.g. Behold TV / SAA713x / SAA7231, Hauppauge, bt8x8 WDM drivers) and the
// video picture, input selection through the crossbar, the TV standard and the tuner channel.
#include "vbifile.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct CaptureDeviceInfo {
    std::string name;
    bool vbi = false;         // has a raw VBI output pin
};
std::vector<CaptureDeviceInfo> captureDevices();
// audio capture devices of the system (sound card inputs: a card without its own sound, bt8x8, is wired to one)
std::vector<std::string> audioInputs();

// AnalogVideoStandard bits (strmif.h) offered in the window
struct CaptureStandard { long bit; const char* name; };
const std::vector<CaptureStandard>& captureStandards();

class Capture {
public:
    Capture();
    ~Capture();
    // builds the graph for device `index` (of captureDevices()); the crossbar and tuner come with it
    bool open(int index, std::string& err);
    // a finished .vbi recording instead of a card (for trying the live decoder): run() plays its frames
    // at the TV frame rate to onFrame, from the start again at the end, over and over; no picture
    // force: the layout the recording's decoder found (Unknown: guessed here from the size)
    bool openFile(const std::string& path, std::string& err, VbiFormat force = VbiFormat::Unknown);
    bool isFile() const;
    int64_t loops() const;                      // file: times it was played to the end
    void close();
    bool isOpen() const;
    std::vector<std::string> inputs() const;   // crossbar video inputs ("Tuner", "Composite", "S-Video")
    int input() const;
    bool setInput(int i);
    bool isTunerInput(int i) const;
    long standard() const;                      // current AnalogVideoStandard
    long standards() const;                     // the ones the decoder supports (bit mask)
    bool setStandard(long bit);                 // closes and builds the graph again when running
    bool hasTuner() const;
    long channel() const;
    bool setChannel(long ch);
    bool locked() const;                        // the decoder is locked to the signal
    // picture sizes the card offers (YUY2) for the current standard; setPictureSize(0, 0) = automatic
    // (the card's own choice; bt8x8: one field, 384x288 - a larger picture makes it lose VBI fields)
    std::vector<std::pair<int, int>> pictureSizes() const;
    void setPictureSize(int w, int h);          // closes and builds the graph again when running
    // the layout (card / chip) the VBI is stored and read in: Unknown = from the driver; one set by hand is taken
    // when it has the card's bytes a line and at least its lines a field (empty lines above fill the rest)
    void setLayout(VbiFormat f);                // closes and builds the graph again when running
    VbiFormat layout() const;                   // as connected
    VbiFormat wantedLayout() const;
    std::pair<int, int> pictureSize() const;    // as connected (0, 0 = none yet)
    // VBI lines per field asked of the driver (first, last; 0, 0 = the driver's own). Some drivers give
    // other lines (lines of the field sync, teletext below the window), others keep theirs: vbiInfo()
    // says what came. Closes and builds the graph again when running.
    void setVbiLines(int first, int last);
    std::pair<int, int> vbiLines() const;       // as connected (first, last), 0, 0 = none yet
    bool bt8x8() const;                         // Bt848/Bt878 WDM driver
    // the decoder's sync modes of a Behold card (driver property set 2BC5F538, as its own property page
    // bhkspex.dll sets them): which = 0 line (H) sync - 0 fast track, 1 VCR, 2 poor TV, 3 auto (default on
    // SAA7231); 1 frame (V) sync - 0 normal (default), 1 fast, 2 immediate, 3 free running. -1 = the card has none
    int syncMode(int which) const;
    bool setSyncMode(int which, int mode);
    // sound: "" none, "card" (the capture filter's own audio output, Behold "Audio PCM"), or the name of
    // one of audioInputs(); played on the default sound output while the capture runs
    bool cardAudio() const;                     // the card has its own audio output
    void setAudio(const std::string& source);   // rebuilds the graph when the card's own sound comes or goes
    const std::string& audio() const;
    bool audioPlaying() const;
    void setMute(bool m);
    bool muted() const;

    bool run(std::string& err);
    void stop();
    bool running() const;
    const VbiGeometry& geom() const;            // VBI layout of the frames given to onFrame
    std::string vbiInfo() const;                // "lines 7-22 / 320-335, 27 MHz, 1440 samples"
    int64_t fields() const;                     // VBI fields received since run()
    int64_t lostFields() const;                 // fields that had no partner (a field of the other parity missing)

    // capture threads: one VBI frame (field 1 lines then field 2 lines, geom()); one video frame (YUY2)
    std::function<void(const uint8_t* frame)> onFrame;
    std::function<void(const uint8_t* yuy2, int w, int h, int stride)> onVideo;

    struct Impl;
private:
    std::unique_ptr<Impl> d;
};

#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr int   kNumVowels   = 5;
static constexpr int   kNumFormants = 3;
static constexpr float kFormantQ    = 0.82f;  // Svf resonance, 0-1
static constexpr float kF1Gain      = 1.0f;
static constexpr float kF2Gain      = 0.6f;
static constexpr float kF3Gain      = 0.25f;
static constexpr float kOutputTrim  = 0.7f;
static constexpr float kSweepBeats  = 4.0f;   // one sweep per bar
static constexpr float kDefaultBeatMs = 500.0f;
static constexpr float kMinBeatMs   = 200.0f;
static constexpr float kMaxBeatMs   = 1500.0f;
static constexpr float kTapTimeoutMs = 3000.0f;
static constexpr float kTempoHoldMs  = 1000.0f;

// Formant centres in Hz: F1, F2, F3
static const float kVowels[kNumVowels][kNumFormants] = {
    {730.0f, 1090.0f, 2440.0f},  // A
    {530.0f, 1840.0f, 2480.0f},  // E
    {270.0f, 2290.0f, 3010.0f},  // I
    {570.0f, 840.0f, 2410.0f},   // O
    {300.0f, 870.0f, 2240.0f},   // U
};

// LED 2 colour per vowel
static const float kVowelColor[kNumVowels][3] = {
    {1.0f, 0.15f, 0.1f},   // A red
    {1.0f, 0.45f, 0.05f},  // E orange
    {1.0f, 0.9f, 0.1f},    // I yellow
    {0.2f, 1.0f, 0.3f},    // O green
    {0.25f, 0.4f, 1.0f},   // U blue
};

// Encoder positions: how far the whole stack is transposed, in semitones
static constexpr int   kNumSizes = 5;
static constexpr float kSizes[kNumSizes] = {-12.0f, -5.0f, 0.0f, 5.0f, 12.0f};

static Svf form_l[kNumFormants];
static Svf form_r[kNumFormants];

static int   size_idx   = 2;    // starts at zero transposition
static float vowel      = 0.0f; // 0 to kNumVowels - 1
static float sweep_phase = 0.0f;
static bool  sweeping   = false;
static float mix        = 0.8f;
static bool  bypass     = false;
static float level      = 0.0f;

// Tap tempo
static float    tempo_ms         = kDefaultBeatMs;
static uint32_t last_tap_ms      = 0;
static float    tap_intervals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static int      tap_count        = 0;
static bool     first_tap        = true;

static bool btn1_prev = false;
static bool btn2_prev = false;
static bool enc_prev  = false;

static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

// Interpolated formant centres for the current vowel position, transposed by
// the voice size
static void UpdateFormants()
{
    float p = ClampF(vowel, 0.0f, (float)(kNumVowels - 1));
    int   i = (int)p;
    if(i >= kNumVowels - 1)
        i = kNumVowels - 2;
    float fr = p - (float)i;

    float shift = powf(2.0f, kSizes[size_idx] / 12.0f);

    for(int f = 0; f < kNumFormants; f++)
    {
        float hz = kVowels[i][f] + fr * (kVowels[i + 1][f] - kVowels[i][f]);
        hz *= shift;
        if(hz > 16000.0f)
            hz = 16000.0f;

        form_l[f].SetFreq(hz);
        form_r[f].SetFreq(hz);
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> vowel, Knob 2 -> mix
    float knob_vowel = pod.knob1.Process() * (float)(kNumVowels - 1);
    mix              = pod.knob2.Process();

    // Edges are detected from the held state: RisingEdge stays true for a whole
    // debounce window, and this callback runs far more often than that
    bool b1        = pod.button1.Pressed();
    bool b2        = pod.button2.Pressed();
    bool enc_down  = pod.encoder.Pressed();
    bool b1_press  = b1 && !btn1_prev;
    bool b2_press  = b2 && !btn2_prev;
    bool enc_press = enc_down && !enc_prev;
    btn1_prev = b1;
    btn2_prev = b2;
    enc_prev  = enc_down;

    // Button 1 -> tap tempo sets the sweep rate, hold stops the sweep
    if(b1_press)
    {
        uint32_t now = System::GetNow();

        if(first_tap)
        {
            first_tap   = false;
            tap_count   = 0;
            last_tap_ms = now;
        }
        else
        {
            uint32_t delta = now - last_tap_ms;

            if(delta > kTapTimeoutMs)
            {
                tap_count   = 0;
                last_tap_ms = now;
            }
            else if(delta > 50)
            {
                if(tap_count < 4)
                {
                    tap_intervals[tap_count++] = (float)delta;
                }
                else
                {
                    tap_intervals[0] = tap_intervals[1];
                    tap_intervals[1] = tap_intervals[2];
                    tap_intervals[2] = tap_intervals[3];
                    tap_intervals[3] = (float)delta;
                }

                float avg = 0.0f;
                for(int i = 0; i < tap_count; i++)
                    avg += tap_intervals[i];

                tempo_ms    = ClampF(avg / (float)tap_count, kMinBeatMs, kMaxBeatMs);
                last_tap_ms = now;
                sweeping    = true;
            }
        }
    }
    if(b1 && pod.button1.TimeHeldMs() > kTempoHoldMs)
    {
        sweeping  = false;
        first_tap = true;
        tap_count = 0;
    }

    // Button 2 -> step to the next vowel, which is what makes it talk
    if(b2_press)
    {
        static int stepped = 0;
        stepped            = (stepped + 1) % kNumVowels;
        sweep_phase        = (float)stepped;
        sweeping           = false;
    }

    // Encoder turn -> voice size, encoder press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
    {
        size_idx = ((size_idx + inc) % kNumSizes + kNumSizes) % kNumSizes;
        UpdateFormants();
    }

    if(enc_press)
        bypass = !bypass;

    float sr         = pod.AudioSampleRate();
    float sweep_inc  = 1.0f / (kSweepBeats * tempo_ms * 0.001f * sr);

    for(size_t i = 0; i < size; i += 2)
    {
        float inl = in[i];
        float inr = in[i + 1];

        if(bypass)
        {
            out[i]     = inl;
            out[i + 1] = inr;
            continue;
        }

        if(sweeping)
        {
            sweep_phase += sweep_inc;
            if(sweep_phase >= (float)(kNumVowels - 1))
                sweep_phase -= (float)(kNumVowels - 1);
            vowel = sweep_phase;
        }
        else
        {
            vowel = knob_vowel;
        }

        UpdateFormants();

        float wl = 0.0f, wr = 0.0f;
        for(int f = 0; f < kNumFormants; f++)
        {
            form_l[f].Process(inl);
            form_r[f].Process(inr);

            float gain = (f == 0) ? kF1Gain : ((f == 1) ? kF2Gain : kF3Gain);
            wl += form_l[f].Band() * gain;
            wr += form_r[f].Band() * gain;
        }

        wl *= kOutputTrim;
        wr *= kOutputTrim;

        out[i]     = inl * (1.0f - mix) + wl * mix;
        out[i + 1] = inr * (1.0f - mix) + wr * mix;

        fonepole(level, fabsf(wl) + fabsf(wr), 0.0005f);
    }

    // LED 1 follows the output level, LED 2 shows the vowel
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float lv = ClampF(level * 2.0f, 0.0f, 1.0f);
        pod.led1.Set(lv * 0.9f, lv * 0.9f, lv);

        int   vi = (int)(vowel + 0.5f);
        if(vi >= kNumVowels)
            vi = kNumVowels - 1;

        float b = 0.3f + 0.7f * lv;
        pod.led2.Set(kVowelColor[vi][0] * b,
                     kVowelColor[vi][1] * b,
                     kVowelColor[vi][2] * b);
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    float sample_rate = pod.AudioSampleRate();

    for(int f = 0; f < kNumFormants; f++)
    {
        form_l[f].Init(sample_rate);
        form_r[f].Init(sample_rate);
        form_l[f].SetRes(kFormantQ);
        form_r[f].SetRes(kFormantQ);
    }

    UpdateFormants();

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}

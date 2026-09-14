#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr size_t kCombMax    = 3072;
static constexpr size_t kAllpassMax = 1024;
static constexpr int    kNumCombs   = 4;
static constexpr int    kNumAllpass = 2;

static constexpr size_t kCombLen[kNumCombs]    = {1557, 1617, 1491, 1422};
static constexpr size_t kAllpassLen[kNumAllpass] = {225, 556};
static constexpr size_t kStereoSpread = 23;     // right channel offset

static constexpr float kMinSwellMs   = 20.0f;   // Knob 1 range
static constexpr float kMaxSwellMs   = 2000.0f;
static constexpr float kSizeMin      = 0.4f;    // Knob 2 range
static constexpr float kSizeMax      = 1.3f;
static constexpr float kFbMin        = 0.60f;
static constexpr float kFbMax        = 0.995f;
static constexpr float kFreezeFb     = 0.999f;
static constexpr float kAllpassG     = 0.7f;
static constexpr float kOnset        = 0.02f;   // level that counts as a note
static constexpr float kOutTrim      = 1.5f;   // makeup gain
static constexpr float kTapeDrive    = 1.3f;   // subtle saturation, flavour not fuzz
static constexpr float kTapeHfHz     = 9000.0f;// gentle roll-off of the tape highs

// Encoder positions: damping in the comb feedback, dark to bright
static constexpr int   kNumTones = 5;
static constexpr float kTones[kNumTones] = {0.85f, 0.65f, 0.45f, 0.28f, 0.12f};

static DelayLine<float, kCombMax>    comb_l[kNumCombs], comb_r[kNumCombs];
static DelayLine<float, kAllpassMax> ap_l[kNumAllpass], ap_r[kNumAllpass];
static float damp_l[kNumCombs], damp_r[kNumCombs];

static int   tone_idx   = 2;
static float swell      = 0.0f;
static float swell_gain = 0.0f;
static float env        = 0.0f;
static float fb         = 0.9f;
static float room       = 1.0f;
static bool  frozen     = false;
static bool  tape       = false;
static bool  bypass     = false;

static OnePole tape_l, tape_r;

static bool btn1_prev = false;
static bool btn2_prev = false;
static bool enc_prev  = false;

static float Saturate(float x)
{
    return tanhf(x);
}

static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

static void ClearTank()
{
    for(int i = 0; i < kNumCombs; i++)
    {
        comb_l[i].Reset();
        comb_r[i].Reset();
        damp_l[i] = damp_r[i] = 0.0f;
    }
    for(int i = 0; i < kNumAllpass; i++)
    {
        ap_l[i].Reset();
        ap_r[i].Reset();
    }
}

// One Schroeder tank: four damped combs in parallel, two allpasses after
static float Tank(float x,
                  DelayLine<float, kCombMax>*    comb,
                  DelayLine<float, kAllpassMax>* ap,
                  float*                         damp)
{
    float y = 0.0f;
    for(int i = 0; i < kNumCombs; i++)
    {
        float c = comb[i].Read();
        damp[i] = c * (1.0f - kTones[tone_idx]) + damp[i] * kTones[tone_idx];
        comb[i].Write(x + damp[i] * fb);
        y += c;
    }
    y *= 0.25f;

    for(int i = 0; i < kNumAllpass; i++)
    {
        float v = ap[i].Read();
        float o = -kAllpassG * y + v;
        ap[i].Write(y + kAllpassG * o);
        y = o;
    }
    return y;
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> swell time, Knob 2 -> size and decay
    float swell_ms = kMinSwellMs * powf(kMaxSwellMs / kMinSwellMs, pod.knob1.Process());
    room = kSizeMin + pod.knob2.Process() * (kSizeMax - kSizeMin);

    float sr = pod.AudioSampleRate();

    // Rise slowly, fall four times faster, so each note blooms in
    float attack  = 1.0f / (swell_ms * 0.001f * sr);
    float release = attack * 4.0f;

    fb = frozen ? kFreezeFb : kFbMin + (room - kSizeMin) / (kSizeMax - kSizeMin)
                                          * (kFbMax - kFbMin);

    for(int i = 0; i < kNumCombs; i++)
    {
        comb_l[i].SetDelay((float)(kCombLen[i] * room));
        comb_r[i].SetDelay((float)(kCombLen[i] * room + kStereoSpread));
    }
    for(int i = 0; i < kNumAllpass; i++)
    {
        ap_l[i].SetDelay((float)(kAllpassLen[i] * room));
        ap_r[i].SetDelay((float)(kAllpassLen[i] * room + kStereoSpread));
    }

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

    // Button 1 -> freeze the tail (latch), Button 2 -> tape saturation on and off
    // Button 1 used to also clear after a long hold, which made a single hold
    // freeze and then wipe the reverb a second later.
    if(b1_press)
        frozen = !frozen;

    if(b2_press)
        tape = !tape;

    // Encoder turn -> tone, encoder press -> bypass
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
        tone_idx = ((tone_idx + inc) % kNumTones + kNumTones) % kNumTones;

    if(enc_press)
        bypass = !bypass;

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

        float mono = 0.5f * (inl + inr);

        // Envelope follower, then the swell
        float mag = fabsf(mono);
        fonepole(env, mag, mag > env ? 0.01f : 0.0002f);

        float target = env > kOnset ? 1.0f : 0.0f;
        fonepole(swell_gain, target, target > swell_gain ? attack : release);

        // Frozen means the tank holds what it has and hears nothing new
        float send = frozen ? 0.0f : mono * swell_gain;

        float wl = Tank(send, comb_l, ap_l, damp_l);
        float wr = Tank(send, comb_r, ap_r, damp_r);

        // Tape stage, off by default: a little tanh glue and the top end rolled
        // off, so the pads sit warmer instead of harder
        if(tape)
        {
            wl = Saturate(wl * kTapeDrive);
            wr = Saturate(wr * kTapeDrive);
            wl = tape_l.Process(wl);
            wr = tape_r.Process(wr);
        }

        out[i]     = Saturate(wl * kOutTrim);
        out[i + 1] = Saturate(wr * kOutTrim);
    }

    // LED 1 follows the swell, LED 2 shows freeze state
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float s = ClampF(swell_gain, 0.0f, 1.0f);
        pod.led1.Set(s * 0.7f, s * 0.7f, s);

        if(frozen)
            pod.led2.Set(0.9f, 0.9f, 1.0f);           // white = frozen
        else if(tape)
            pod.led2.Set(1.0f, 0.55f, 0.05f);          // amber = tape on
        else
            pod.led2.Set(0.05f, 0.05f, 0.35f + 0.65f * s);  // blue = clean
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    float sample_rate = pod.AudioSampleRate();

    for(int i = 0; i < kNumCombs; i++)
    {
        comb_l[i].Init();
        comb_r[i].Init();
    }
    for(int i = 0; i < kNumAllpass; i++)
    {
        ap_l[i].Init();
        ap_r[i].Init();
    }
    ClearTank();

    tape_l.Init();
    tape_r.Init();
    tape_l.SetFrequency(kTapeHfHz / sample_rate);
    tape_r.SetFrequency(kTapeHfHz / sample_rate);

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}

#include "daisysp.h"
#include "daisy_pod.h"

using namespace daisysp;
using namespace daisy;

static DaisyPod pod;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
static constexpr int    kMaxLoopSeconds = 60;
static constexpr size_t kMaxLoopSamples = (size_t)kMaxLoopSeconds * 48000;
static constexpr float  kMaxLevel       = 1.2f;
static constexpr float  kMaxFeedback    = 0.98f;
static constexpr float  kEraseHoldMs    = 2000.0f;
static constexpr float  kFlashDecay     = 0.0008f;

// Encoder positions: normal, half speed, reverse, double speed
static constexpr int   kNumSpeeds     = 4;
static constexpr float kRates[kNumSpeeds] = {1.0f, 0.5f, -1.0f, 2.0f};

enum State
{
    STATE_EMPTY = 0,  // nothing recorded yet
    STATE_REC,        // first pass, this sets the loop length
    STATE_PLAY,
    STATE_DUB,        // overdub, old layers fade by the feedback amount
    STATE_STOP
};

// Two channels of loop audio, in external SDRAM
static float DSY_SDRAM_BSS loop_l[kMaxLoopSamples];
static float DSY_SDRAM_BSS loop_r[kMaxLoopSamples];

static int    state      = STATE_EMPTY;
static size_t loop_len   = 0;      // set by the first record pass
static size_t write_idx  = 0;      // integer write head
static float  read_pos   = 0.0f;   // fractional read head
static float  rate       = 1.0f;
static int    speed      = 0;
static float  level      = 0.9f;
static float  feedback   = 0.9f;
static bool   bypass     = false;
static float  flash      = 0.0f;
static bool   btn1_prev  = false;
static bool   btn2_prev  = false;
static bool   enc_prev   = false;

static float ClampF(float x, float lo, float hi)
{
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return x;
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    pod.ProcessAnalogControls();
    pod.ProcessDigitalControls();

    // Knob 1 -> loop level, Knob 2 -> how fast old layers fade on overdub
    level    = pod.knob1.Process() * kMaxLevel;
    feedback = pod.knob2.Process() * kMaxFeedback;

    // Edges are detected from the held state: RisingEdge and FallingEdge stay
    // true for a whole debounce window, and this callback runs far more often
    bool b1       = pod.button1.Pressed();
    bool b2       = pod.button2.Pressed();
    bool enc_down = pod.encoder.Pressed();

    bool b1_press  = b1 && !btn1_prev;
    bool b2_press  = b2 && !btn2_prev;
    bool enc_press = enc_down && !enc_prev;

    btn1_prev = b1;
    btn2_prev = b2;
    enc_prev  = enc_down;

    // Button 1 -> transport: record, then play, then in and out of overdub
    if(b1_press)
    {
        switch(state)
        {
            case STATE_EMPTY:
                write_idx = 0;
                read_pos  = 0.0f;
                loop_len  = 0;
                state     = STATE_REC;
                break;
            case STATE_REC:
                loop_len  = write_idx;
                read_pos  = 0.0f;
                state     = loop_len > 0 ? STATE_PLAY : STATE_EMPTY;
                break;
            case STATE_PLAY: state = STATE_DUB; break;
            case STATE_DUB: state = STATE_PLAY; break;
            case STATE_STOP:
                read_pos = 0.0f;
                state    = STATE_PLAY;
                break;
        }
    }

    // Button 2 -> stop and restart, hold to erase
    if(b2_press)
    {
        if(state == STATE_PLAY || state == STATE_DUB || state == STATE_REC)
            state = STATE_STOP;
        else if(state == STATE_STOP)
            state = STATE_EMPTY;
    }
    if(b2 && pod.button2.TimeHeldMs() > kEraseHoldMs)
    {
        state    = STATE_EMPTY;
        loop_len = 0;
        write_idx = 0;
        read_pos  = 0.0f;
    }

    // Encoder turn -> speed and direction, encoder press -> bypass the loop
    int32_t inc = pod.encoder.Increment();
    if(inc != 0)
    {
        speed = ((speed + inc) % kNumSpeeds + kNumSpeeds) % kNumSpeeds;
        rate  = kRates[speed];
    }

    if(enc_press)
        bypass = !bypass;

    bool running = !bypass && (state == STATE_PLAY || state == STATE_DUB);

    for(size_t i = 0; i < size; i += 2)
    {
        float inl = in[i];
        float inr = in[i + 1];

        if(state == STATE_REC)
        {
            // First pass: write the loop and pass the dry signal through
            loop_l[write_idx] = inl;
            loop_r[write_idx] = inr;
            write_idx++;

            if(write_idx >= kMaxLoopSamples)
            {
                // Out of memory, close the loop and roll it
                loop_len  = write_idx;
                write_idx = 0;
                read_pos  = 0.0f;
                state     = STATE_PLAY;
            }

            out[i]     = inl;
            out[i + 1] = inr;
            continue;
        }

        if(!running || loop_len == 0)
        {
            out[i]     = inl;
            out[i + 1] = inr;
            continue;
        }

        size_t i0 = (size_t)read_pos;
        float  fr = read_pos - (float)i0;
        size_t i1 = i0 + 1;
        if(i1 >= loop_len)
            i1 -= loop_len;

        float ol = loop_l[i0];
        float orr = loop_r[i0];

        if(state == STATE_DUB && rate == 1.0f)
        {
            // Sound on sound: the old layer fades by the feedback amount
            ol  = ol * feedback + inl;
            orr = orr * feedback + inr;
            loop_l[i0] = ol;
            loop_r[i0] = orr;
        }

        float pl = ol + fr * (loop_l[i1] - ol);
        float pr = orr + fr * (loop_r[i1] - orr);

        out[i]     = inl + pl * level;
        out[i + 1] = inr + pr * level;

        read_pos += rate;
        if(read_pos >= (float)loop_len)
        {
            read_pos -= (float)loop_len;
            flash = 1.0f;
        }
        else if(read_pos < 0.0f)
        {
            read_pos += (float)loop_len;
            flash = 1.0f;
        }

        flash -= flash * kFlashDecay;
    }

    // LED 1 is the transport state, LED 2 pulses at the top of the loop
    if(bypass)
    {
        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.led2.Set(0.0f, 0.0f, 0.0f);
    }
    else
    {
        float rl, gl, bl;
        switch(state)
        {
            case STATE_EMPTY: rl = 0.0f; gl = 0.0f; bl = 0.0f; break;
            case STATE_REC: rl = 1.0f; gl = 0.0f; bl = 0.0f; break;
            case STATE_PLAY: rl = 0.0f; gl = 1.0f; bl = 0.0f; break;
            case STATE_DUB: rl = 0.2f; gl = 0.4f; bl = 1.0f; break;
            default: rl = 0.25f; gl = 0.25f; bl = 0.25f; break;
        }
        pod.led1.Set(rl, gl, bl);

        float f = flash;
        if(rate < 0.0f)
            pod.led2.Set(f, f * 0.45f, 0.0f);  // amber, running backwards
        else
            pod.led2.Set(0.0f, f * 0.8f, f);   // cyan, forwards
    }

    pod.UpdateLeds();
}

int main(void)
{
    pod.Init();
    pod.SetAudioBlockSize(4);

    // The .sdram_bss section is not guaranteed to come up cleared
    for(size_t i = 0; i < kMaxLoopSamples; i++)
        loop_l[i] = loop_r[i] = 0.0f;

    pod.StartAdc();
    pod.StartAudio(AudioCallback);

    while(1) {}
}

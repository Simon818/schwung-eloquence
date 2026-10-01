/* libflite.so.1 for Schwung's screen reader, speaking with Eloquence.
 *
 * Schwung links Flite and calls five of its functions: flite_init,
 * register_cmu_us_kal, feat_set_float, flite_text_to_wave and delete_wave.
 * This library is those five, answered by an ECI engine: OpenEVV's
 * libeci.so.1, or any other implementation of IBM's interface put in its
 * place. The engine is loaded the first time something is spoken.
 *
 * Its files are in /data/UserData/eloquence:
 *     libeci.so.1     the engine
 *     eloquence.json  settings, read once: "voice", "wpm", "pitch", "inflection",
 *                     "phrase_prediction", "log_text"
 *     dict/regexp.dic text replacements, read again whenever it changes
 *     dict/ENU*.dic   pronunciation dictionaries, if added, read once
 *     eloquence.log   what happened, started afresh each time Move starts
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "eci.h"

#define ELOQ   "/data/UserData/eloquence"
#define EXPORT __attribute__((visibility("default")))

/* entry.sh looks for this to tell our libraries from Schwung's. */
__attribute__((used)) static const char marker[] = "schwung-eloquence";

/* ---- Flite's types -------------------------------------------------------
 * As much of Flite 2.2's cst_voice and cst_wave as Schwung reads; their
 * layouts must match. cst_features is ours: Schwung only hands the pointer
 * back to feat_set_float. */

typedef struct {
    volatile float stretch;     /* duration_stretch: 1 / speed */
    volatile float f0;          /* int_f0_target_mean: pitch in Hz */
} cst_features;

typedef struct {
    const char   *name;
    cst_features *features;
    void         *ffunctions, *utt_init;
} cst_voice;

typedef struct {
    const char *type;
    int         sample_rate, num_samples, num_channels;
    short      *samples;
} cst_wave;

/* Schwung's pitch setting runs from 80 to 180 Hz with 110 as normal, which
   we take to mean the voice's own pitch. */
#define NORMAL_PITCH 110.0f

static cst_features features = { 1.0f, NORMAL_PITCH };
static cst_voice    voice    = { "eloquence", &features, 0, 0 };

/* ---- audio ---------------------------------------------------------------
 * The engine runs at its native 11025 Hz and we raise that to 44100 below:
 * OpenEVV's own 44.1 kHz output costs ~650 ms a phrase on the Move, and
 * doing it here costs a few. */

#define OUTPUT_RATE 44100
#define FRAME       4096        /* samples the engine hands over at a time */
#define SILENT      64          /* quieter than this counts as silence */
static int rate = 11025;        /* what the engine actually gives us */
/* Schwung drops anything longer than its 12 s buffer outright; stop short. */
#define MAX_SAMPLES (rate * 119 / 10)
/* Eloquence ends each utterance with ~380 ms of silence; keep 40 ms. */
#define TAIL_KEEP   (rate * 40 / 1000)

/* ---- the engine, its settings and the log ------------------------------- */

static struct {
    ECIHand (*New)(void);
    void    (*RegisterCallback)(ECIHand, ECICallback, void *);
    int     (*SetOutputBuffer)(ECIHand, int, short *);
    int     (*SetParam)(ECIHand, int, int);
    int     (*GetParam)(ECIHand, int);
    int     (*CopyVoice)(ECIHand, int, int);
    int     (*SetVoiceParam)(ECIHand, int, int, int);
    int     (*GetVoiceParam)(ECIHand, int, int);
    int     (*AddText)(ECIHand, const void *);
    int     (*Synthesize)(ECIHand);
    int     (*Synchronize)(ECIHand);
    ECIDictHand (*NewDict)(ECIHand);
    int     (*LoadDict)(ECIHand, ECIDictHand, int, const void *);
    int     (*SetDict)(ECIHand, ECIDictHand);
} eci;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int     state;           /* 0 not tried yet, 1 ready, -1 failed */
static ECIHand hand;
static short   frame[FRAME];

static int cfg_voice = 1;       /* Eloquence's preset voice, 1 to 8 */
static int cfg_wpm, cfg_pitch;  /* at normal speed and pitch; 0: the voice's own */
static int cfg_inflection = -1; /* pitch fluctuation, 0 to 100; -1: the voice's own */
static int cfg_pp;              /* phrase prediction, 0 or 1; off, unlike the engine */
static int cfg_log_text;        /* 1: log every text spoken */
static int base_wpm, base_pitch;
static int applied_wpm = -1, applied_pitch = -1;

static void say(const char *fmt, ...)
{
    static FILE *out;
    static int lines;
    va_list ap;

    if (!out && !(out = fopen(ELOQ "/eloquence.log", "w")))
        return;
    if (lines++ > 1000)
        return;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}

/* Not a JSON parser: it finds "key" and reads the number after the colon.
   The file is flat and ours, and that is all it needs. */
static void read_int(const char *json, const char *key, int *value)
{
    char quoted[32];
    const char *p;

    snprintf(quoted, sizeof quoted, "\"%s\"", key);
    if ((p = strstr(json, quoted)) && (p = strchr(p, ':')))
        *value = atoi(p + 1);
}

static void load_config(void)
{
    char json[4096] = "";
    FILE *f = fopen(ELOQ "/eloquence.json", "r");

    if (f) {
        json[fread(json, 1, sizeof json - 1, f)] = 0;
        fclose(f);
    }
    read_int(json, "voice", &cfg_voice);
    read_int(json, "wpm", &cfg_wpm);
    read_int(json, "pitch", &cfg_pitch);
    read_int(json, "inflection", &cfg_inflection);
    read_int(json, "phrase_prediction", &cfg_pp);
    read_int(json, "log_text", &cfg_log_text);
    if (cfg_voice < 1 || cfg_voice > 8)
        cfg_voice = 1;
    if (cfg_inflection > 100)
        cfg_inflection = 100;
    if (cfg_pp != 1)
        cfg_pp = 0;
}

/* Schwung's synthesis thread inherits real-time priority and Move's audio
   core from the thread that started it. Give both up before doing any work,
   so the engine (and the thread it makes) never competes with Move's audio. */
static void leave_realtime(void)
{
    int policy;
    struct sched_param param, normal = { 0 };
    cpu_set_t cores;

    if (pthread_getschedparam(pthread_self(), &policy, &param) == 0
        && policy != SCHED_OTHER)
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);

    CPU_ZERO(&cores);           /* core 3 is Move's audio core */
    CPU_SET(0, &cores);
    CPU_SET(1, &cores);
    CPU_SET(2, &cores);
    pthread_setaffinity_np(pthread_self(), sizeof cores, &cores);
}

#define BIND(name) \
    if (!(*(void **)&eci.name = dlsym(lib, "eci" #name))) { \
        say("the engine has no eci" #name); \
        return 0; \
    }

/* The community pronunciation dictionaries, whichever of them are there:
   https://github.com/eigencrow/IBMTTSDictionaries. Their names are theirs. */
static void load_dictionaries(void)
{
    static const struct { const char *name; int volume; } dicts[] = {
        { "ENUmain.dic", eciMainDict },
        { "ENURoot.dic", eciRootDict },
        { "ENUabbr.dic", eciAbbvDict },
    };
    ECIDictHand set = 0;
    char path[256];
    struct timespec t0, t1;
    unsigned i;
    int rc;

    for (i = 0; i < sizeof dicts / sizeof dicts[0]; i++) {
        snprintf(path, sizeof path, ELOQ "/dict/%s", dicts[i].name);
        if (access(path, R_OK) != 0)
            continue;
        if (!set && !(set = eci.NewDict(hand))) {
            say("the engine would not make a dictionary");
            return;
        }
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rc = eci.LoadDict(hand, set, dicts[i].volume, path);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (rc == eciDictNoError)
            say("%s loaded in %ld ms", dicts[i].name,
                (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        else
            say("%s could not be loaded (dictionary error %d)", dicts[i].name, rc);
    }
    if (set)
        eci.SetDict(hand, set);
}

static int load_engine(void)
{
    static const int rates[] = { 8000, 11025, 22050, 16000, 32000, 44100, 48000 };
    void *lib = dlopen(ELOQ "/libeci.so.1", RTLD_NOW | RTLD_LOCAL);
    int r;

    if (!lib) {
        say("could not load the engine: %s", dlerror());
        return 0;
    }
    BIND(New) BIND(RegisterCallback) BIND(SetOutputBuffer) BIND(SetParam)
    BIND(GetParam) BIND(CopyVoice) BIND(SetVoiceParam) BIND(GetVoiceParam)
    BIND(AddText) BIND(Synthesize) BIND(Synchronize) BIND(NewDict) BIND(LoadDict)
    BIND(SetDict)

    if ((hand = eci.New()) == NULL_ECI_HAND) {
        say("the engine would not start");
        return 0;
    }
    /* Annotations on, for phrase prediction, which has no setting of its
       own. A backtick in Schwung's text is a space, so it cannot be one. */
    eci.SetParam(hand, eciInputType, 1);
    eci.SetParam(hand, eciSampleRate, 1);       /* 11025 Hz, by its number */
    r = eci.GetParam(hand, eciSampleRate);
    rate = r >= 0 && r <= 6 ? rates[r] : r >= 8000 ? r : 11025;

    eci.CopyVoice(hand, cfg_voice, 0);
    /* Before real-world units, which refuse an inflection of 0. */
    eci.SetParam(hand, eciRealWorldUnits, 0);
    if (cfg_inflection >= 0)
        eci.SetVoiceParam(hand, 0, eciPitchFluctuation, cfg_inflection);
    eci.SetParam(hand, eciRealWorldUnits, 1);   /* speed in wpm, pitch in Hz */
    base_wpm   = cfg_wpm   > 0 ? cfg_wpm   : eci.GetVoiceParam(hand, 0, eciSpeed);
    base_pitch = cfg_pitch > 0 ? cfg_pitch : eci.GetVoiceParam(hand, 0, eciPitchBaseline);
    if (base_wpm <= 0)
        base_wpm = 176;
    if (base_pitch <= 0)
        base_pitch = 118;
    say("engine ready: voice %d, %d wpm and %d Hz at normal speed and pitch, "
        "inflection %d, phrase prediction %s, %d Hz audio",
        cfg_voice, base_wpm, base_pitch, eci.GetVoiceParam(hand, 0, eciPitchFluctuation),
        cfg_pp ? "on" : "off", rate);
    load_dictionaries();
    return 1;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Schwung's speed and pitch, relative to the voice's own. */
static void apply_voice(void)
{
    float stretch = features.stretch > 0.0f ? features.stretch : 1.0f;
    int wpm   = clampi((int)(base_wpm / stretch + 0.5f), 70, 1200);
    int pitch = clampi((int)(base_pitch * features.f0 / NORMAL_PITCH + 0.5f), 40, 422);

    if (wpm != applied_wpm) {
        eci.SetVoiceParam(hand, 0, eciSpeed, wpm);
        applied_wpm = wpm;
    }
    if (pitch != applied_pitch) {
        eci.SetVoiceParam(hand, 0, eciPitchBaseline, pitch);
        applied_pitch = pitch;
    }
}

/* ---- text: Schwung's UTF-8 into the Windows-1252 Eloquence reads --------- */

static unsigned char to_cp1252(unsigned cp)
{
    if (cp < 0x20)
        return ' ';
    if (cp < 0x80 || (cp >= 0xa0 && cp <= 0xff))
        return (unsigned char)cp;
    switch (cp) {
    case 0x20ac: return 0x80;   /* euro */
    case 0x2026: return 0x85;   /* ellipsis */
    case 0x2018: case 0x2019: return '\'';
    case 0x201c: case 0x201d: return '"';
    case 0x2022: return 0x95;   /* bullet */
    case 0x2013: return 0x96;   /* en dash */
    case 0x2014: return 0x97;   /* em dash */
    case 0x2122: return 0x99;   /* trade mark */
    case 0x266d: return 'b';    /* flat */
    case 0x266f: return '#';    /* sharp */
    }
    return ' ';
}

/* Control characters become spaces, except a tab when keep_tab is set. */
static void utf8_to_cp1252(const char *in, char *out, size_t n, int keep_tab)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t o = 0;

    while (*s && o + 1 < n) {
        unsigned cp;
        int more;

        if (*s < 0x80)       { cp = *s;        more = 0; }
        else if (*s >= 0xf0) { cp = *s & 0x07; more = 3; }
        else if (*s >= 0xe0) { cp = *s & 0x0f; more = 2; }
        else if (*s >= 0xc0) { cp = *s & 0x1f; more = 1; }
        else                 { s++; continue; }     /* a stray continuation byte */
        s++;
        while (more-- > 0 && (*s & 0xc0) == 0x80)
            cp = (cp << 6) | (*s++ & 0x3f);
        out[o++] = keep_tab && cp == '\t' ? '\t' : (char)to_cp1252(cp);
    }
    out[o] = 0;
}

/* ---- text fixes ------------------------------------------------------------
 * dict/regexp.dic holds regular-expression replacements in the shape of
 * OpenEVV's dictionaries: one a line, the pattern, a tab, the replacement.
 * Unlike those, nothing is trimmed: every character after the tab is
 * replaced in, spaces included. Lines starting with # are comments and
 * blank lines are skipped. Patterns are PCRE2's, which reads the syntax of
 * the NVDA add-ons' rules, and (?i) at the start makes one ignore case. In
 * a replacement, \1, \g<1> and \g<name> are what a group matched, as in
 * Python, and a group that did not match is empty. Rules run in order over
 * the Windows-1252 text the engine is given. */

#define REGEXP_DIC ELOQ "/dict/regexp.dic"
#define MAX_RULES  1000
#define TEXT_MAX   16384

static struct {
    pcre2_code *code;
    char       *repl;           /* in PCRE2's replacement syntax */
    int         line;
} rules[MAX_RULES];
static int nrules;

/* Python's \1, \g<1> and \g<name> into PCRE2's ${1} and ${name}; a $ is
   doubled, and \\ is one backslash. */
static char *convert_repl(const char *in)
{
    char *out = malloc(strlen(in) * 3 + 1);
    size_t o = 0;

    if (!out)
        return 0;
    while (*in) {
        const char *end;

        if (in[0] == '\\' && in[1] >= '0' && in[1] <= '9') {
            int digits = in[2] >= '0' && in[2] <= '9' ? 2 : 1;
            o += (size_t)sprintf(out + o, "${%.*s}", digits, in + 1);
            in += 1 + digits;
        } else if (in[0] == '\\' && in[1] == 'g' && in[2] == '<'
                   && (end = strchr(in + 3, '>'))) {
            o += (size_t)sprintf(out + o, "${%.*s}", (int)(end - in - 3), in + 3);
            in = end + 1;
        } else if (in[0] == '\\' && in[1] == '\\') {
            out[o++] = '\\';
            in += 2;
        } else {
            if (*in == '$')
                out[o++] = '$';
            out[o++] = *in++;
        }
    }
    out[o] = 0;
    return out;
}

static void add_rule(char *line, int lineno)
{
    char *tab = strchr(line, '\t');
    int err;
    PCRE2_SIZE erroff;
    pcre2_code *code;

    if (*line == 0 || *line == '#')
        return;
    if (!tab) {
        say("regexp.dic line %d: no tab between the pattern and the replacement", lineno);
        return;
    }
    if (nrules == MAX_RULES) {
        say("regexp.dic line %d: more than %d rules; ignored", lineno, MAX_RULES);
        return;
    }
    *tab = 0;
    code = pcre2_compile((PCRE2_SPTR)line, PCRE2_ZERO_TERMINATED, 0, &err, &erroff, 0);
    if (!code) {
        PCRE2_UCHAR msg[160];
        pcre2_get_error_message(err, msg, sizeof msg);
        say("regexp.dic line %d: %s, at character %d", lineno, msg, (int)erroff + 1);
        return;
    }
    rules[nrules].code = code;
    rules[nrules].repl = convert_repl(tab + 1);
    rules[nrules].line = lineno;
    if (rules[nrules].repl)
        nrules++;
    else
        pcre2_code_free(code);
}

/* Read regexp.dic again if it has changed since last time. */
static void load_fixes(void)
{
    static time_t mtime = -1;
    static off_t size = -1;
    struct stat st;
    char line[8192], text[8192];
    FILE *f;
    int lineno = 0;

    if (stat(REGEXP_DIC, &st) != 0) {
        st.st_mtime = 0;            /* no file: no rules */
        st.st_size = 0;
    }
    if (st.st_mtime == mtime && st.st_size == size)
        return;
    mtime = st.st_mtime;
    size = st.st_size;

    while (nrules > 0) {
        nrules--;
        pcre2_code_free(rules[nrules].code);
        free(rules[nrules].repl);
    }
    if (!(f = fopen(REGEXP_DIC, "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        utf8_to_cp1252(line, text, sizeof text, 1);
        add_rule(text, ++lineno);
    }
    fclose(f);
    say("regexp.dic: %d rules", nrules);
}

/* Every rule in turn. Returns text itself if nothing matched. */
static const char *fix_text(const char *text)
{
    static char bufs[2][TEXT_MAX];
    const char *cur = text;
    int which = 0, i;

    for (i = 0; i < nrules; i++) {
        PCRE2_SIZE len = TEXT_MAX;
        int rc = pcre2_substitute(rules[i].code, (PCRE2_SPTR)cur, PCRE2_ZERO_TERMINATED, 0,
                                  PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_UNSET_EMPTY,
                                  0, 0, (PCRE2_SPTR)rules[i].repl, PCRE2_ZERO_TERMINATED,
                                  (PCRE2_UCHAR *)bufs[which], &len);
        if (rc > 0) {
            cur = bufs[which];
            which ^= 1;
        } else if (rc < 0) {
            PCRE2_UCHAR msg[160];
            pcre2_get_error_message(rc, msg, sizeof msg);
            say("regexp.dic line %d: %s", rules[i].line, msg);
        }
    }
    return cur;
}

/* ---- collecting the engine's samples -------------------------------------- */

typedef struct {
    short *samples;
    int    count, room;
} collect;

static int ECICALL on_message(ECIHand h, ECIMessage msg, int param, void *data)
{
    collect *c = data;
    int take = param;

    (void)h;
    if (msg != eciWaveformBuffer || !c)
        return eciDataProcessed;
    if (c->count + take > MAX_SAMPLES)
        take = MAX_SAMPLES - c->count;
    if (take <= 0)
        return eciDataProcessed;    /* past the cap: let it finish unheard */

    if (c->count + take > c->room) {
        int room = c->room ? c->room * 2 : rate * 2;
        short *grown;

        if (room > MAX_SAMPLES)
            room = MAX_SAMPLES;
        if (!(grown = realloc(c->samples, (size_t)room * sizeof *grown)))
            return eciDataProcessed;
        c->samples = grown;
        c->room = room;
    }
    memcpy(c->samples + c->count, frame, (size_t)take * sizeof *frame);
    c->count += take;
    return eciDataProcessed;
}

/* ---- raising 11025 Hz to 44100 ---------------------------------------------
 * A polyphase windowed sinc: 128 taps, 32 for each of the 4 output phases,
 * cut at 90% of the input's Nyquist under a Kaiser window. That keeps
 * everything the upsampling adds 83 dB down, as good as OpenEVV's own, for
 * about 5 million multiplies per 10 seconds of speech. */

#define UP      4
#define UP_TAPS 32
static float up_h[UP][UP_TAPS];
static int   up_ready;

static double bessel_i0(double x)
{
    double term = 1.0, sum = 1.0, half = x / 2.0;
    int k;

    for (k = 1; k < 40 && term > sum * 1e-12; k++) {
        term *= (half / k) * (half / k);
        sum += term;
    }
    return sum;
}

static void up_design(void)
{
    const int    taps = UP * UP_TAPS;
    const double fc = 0.9 * 0.5 / UP;       /* cycles per output sample */
    const double beta = 9.0, mid = (taps - 1) / 2.0;
    int n;

    for (n = 0; n < taps; n++) {
        double t = n - mid, x = t / mid;
        double sinc = t == 0 ? 1.0 : sin(2 * M_PI * fc * t) / (2 * M_PI * fc * t);
        double window = bessel_i0(beta * sqrt(1.0 - x * x)) / bessel_i0(beta);
        up_h[n % UP][n / UP] = (float)(2.0 * fc * sinc * window * UP);
    }
    up_ready = 1;
}

/* in[0..len) at the engine's rate, out[0..len*UP) at four times it. */
static void upsample(const short *in, int len, short *out)
{
    const int delay = UP * UP_TAPS / 2;     /* the filter's own, in output samples */
    int m;

    for (m = 0; m < len * UP; m++) {
        int at = m + delay, i = at / UP, phase = at % UP, j;
        float acc = 0.0f;

        for (j = 0; j < UP_TAPS; j++)
            if (i - j >= 0 && i - j < len)
                acc += in[i - j] * up_h[phase][j];
        out[m] = acc > 32767.0f ? 32767 : acc < -32768.0f ? -32768 : (short)lrintf(acc);
    }
}

/* ---- the five Flite functions --------------------------------------------
 * flite_init, register_cmu_us_kal and feat_set_float are called from Move's
 * audio path, so they only hand back or store static data. */

EXPORT int flite_init(void)
{
    return 0;
}

EXPORT cst_voice *register_cmu_us_kal(const char *voxdir)
{
    (void)voxdir;
    return &voice;
}

EXPORT void feat_set_float(cst_features *f, const char *name, float v)
{
    if (f != &features || !name)
        return;
    if (strcmp(name, "duration_stretch") == 0)
        features.stretch = v;
    else if (strcmp(name, "int_f0_target_mean") == 0)
        features.f0 = v;
}

/* Called on Schwung's synthesis thread, one utterance at a time. */
EXPORT cst_wave *flite_text_to_wave(const char *text, cst_voice *v)
{
    static char converted[8192], annotated[TEXT_MAX + 8];
    const char *fixed;
    char *tick;
    collect c = { 0, 0, 0 };
    cst_wave *w;
    int end;

    (void)v;
    if (!text)
        return 0;
    leave_realtime();

    pthread_mutex_lock(&lock);
    if (state == 0) {
        load_config();
        state = load_engine() ? 1 : -1;
    }
    if (state != 1) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    apply_voice();
    utf8_to_cp1252(text, converted, sizeof converted, 0);
    for (tick = converted; (tick = strchr(tick, '`')); )
        *tick = ' ';
    load_fixes();
    fixed = fix_text(converted);
    if (cfg_log_text && fixed != converted)
        say("text: '%s' -> '%s'", converted, fixed);
    else if (cfg_log_text)
        say("text: '%s'", converted);
    /* Every time, as the NVDA add-ons do, so nothing can leave it changed. */
    snprintf(annotated, sizeof annotated, "`pp%d %s", cfg_pp, fixed);
    fixed = annotated;

    eci.RegisterCallback(hand, on_message, &c);
    if (!eci.SetOutputBuffer(hand, FRAME, frame) || !eci.AddText(hand, fixed)
        || !eci.Synthesize(hand)) {
        eci.RegisterCallback(hand, on_message, 0);
        pthread_mutex_unlock(&lock);
        say("the engine refused: '%s'", text);
        free(c.samples);
        return 0;
    }
    eci.Synchronize(hand);
    eci.RegisterCallback(hand, on_message, 0);
    pthread_mutex_unlock(&lock);

    if (c.count >= MAX_SAMPLES)
        say("cut at 11.9 seconds: '%.60s'", text);

    /* Trim the trailing silence to a short tail. */
    for (end = c.count; end > 0 && abs(c.samples[end - 1]) <= SILENT; end--)
        ;
    end += TAIL_KEEP;
    if (end > c.count)
        end = c.count;
    if (end == 0 || !(w = malloc(sizeof *w))) {
        free(c.samples);
        return 0;
    }
    w->type = "riff";
    w->num_channels = 1;
    w->sample_rate = rate;
    w->num_samples = end;
    w->samples = c.samples;

    /* Schwung can only repeat samples to reach 44.1 kHz; do it properly. */
    if (rate * UP == OUTPUT_RATE) {
        short *up = malloc((size_t)end * UP * sizeof *up);
        if (up) {
            if (!up_ready)
                up_design();
            upsample(c.samples, end, up);
            free(c.samples);
            w->samples = up;
            w->num_samples = end * UP;
            w->sample_rate = OUTPUT_RATE;
        }
    }
    return w;
}

EXPORT void delete_wave(cst_wave *w)
{
    if (w) {
        free(w->samples);
        free(w);
    }
}

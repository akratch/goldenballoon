/*
 * web_audio_worklet.c — web-only AudioWorklet output backend (see the header).
 *
 * Vendored + adapted from mgb64/src/platform/web_audio_worklet.c (identifiers
 * renamed mgb64 -> mdkr). All logic lives in EM_JS bridges to Web Audio; on
 * native this is an empty translation unit. mdkr64's synth/mixer stages are
 * unchanged and upstream — this backend is a leaf swap at the osAiSetNextBuffer
 * boundary (audi_port_dkr.c), web-only, behind __EMSCRIPTEN__.
 */
/* Non-empty on native (this whole file is web-only). */
typedef int web_audio_worklet_tu_guard;

#ifdef __EMSCRIPTEN__

#include "web_audio_worklet.h"

#include <emscripten/em_js.h>

/*
 * The worklet processor source, injected as a Blob module so no extra deploy
 * file is needed. It:
 *   - keeps a 1-second ring buffer per channel;
 *   - on a 'pcm' message, converts interleaved s16 -> float and fills the ring;
 *   - in process(), emits one render quantum, resampling srcRate -> the context
 *     rate with a fractional read pointer + linear interpolation (ratio == 1,
 *     i.e. a plain copy, when the browser honours the requested 22050 context);
 *   - reports its live ring count back every 3 quanta so the main thread can
 *     estimate occupancy without shared memory.
 */
EM_JS(int, webAudioOutputInit, (int srcRate, int channels), {
    try {
        var AC = window.AudioContext || window.webkitAudioContext;
        if (!AC) return 0;
        /* iOS Safari accepts a non-hardware sampleRate request and then
         * renders SILENCE through the speaker route (long-standing WebKit
         * defect; desktop browsers resample internally instead). Never
         * request the 22050 source rate on Apple touch devices — create at
         * the hardware default and let the worklet's fractional-read
         * resampler (already the fallback path) convert. */
        var iosLike = false;
        try {
            iosLike = /iP(hone|ad|od)/.test(navigator.userAgent) ||
                (navigator.platform === 'MacIntel' &&
                 navigator.maxTouchPoints > 1);
        } catch (e0) {}
        var ctx = null;
        if (!iosLike) {
            try { ctx = new AC({ sampleRate: srcRate, latencyHint: 'interactive' }); }
            catch (e) { ctx = null; }
        }
        if (!ctx) {
            try { ctx = new AC({ latencyHint: 'interactive' }); }
            catch (e1) { return 0; }
        }
        if (!ctx || !ctx.audioWorklet) { try { if (ctx) ctx.close(); } catch (e2) {} return 0; }

        var A = {
            ctx: ctx, node: null, ready: false, failed: false, pending: [],
            posted: 0, statRing: 0, statTime: 0, postedAtStat: 0, under: 0,
            /* droppedFrames only accounts for worklet-ring eviction. Pending
             * module-load drops are reported synchronously to C by Push(),
             * because replaying their cumulative worklet status would count
             * them twice. */
            droppedFrames: 0, droppedTotal: 0, droppedSeen: 0, pendingDrops: 0,
            pendingDroppedFrames: 0, recoveries: 0, recoveryFrames: 0,
            recoverySamples: 0, completedRecoveries: 0,
            srcRate: srcRate, chans: channels, closed: false,
            shutdownComplete: false
        };

        var src =
            "class MDKRRing extends AudioWorkletProcessor{" +
              "constructor(o){super();" +
                "var p=(o&&o.processorOptions)||{};" +
                /* 0.4 s ring: the overflow policy drops the OLDEST samples,
                   so the ring capacity IS the hard upper bound on audio
                   latency in every failure mode (suspension backlogs, main
                   thread stalls). 1 s was needlessly generous and let a bad
                   state feel like a huge delay. */
                "this.cap=Math.max(8192,(((p.srcRate||22050)*2)/5)|0);" +
                "this.ratio=(p.srcRate||22050)/sampleRate;" +    /* src frames per out frame */
                "this.L=new Float32Array(this.cap);this.R=new Float32Array(this.cap);" +
                "this.r=0;this.w=0;this.count=0;this.frac=0;this.under=0;this.q=0;" +
                "this.drop=0;this.fade=0;this.recoveries=0;this.recoverySamples=0;this.completedRecoveries=0;this.fromL=0;this.fromR=0;this.lastL=0;this.lastR=0;" +
                "this.port.onmessage=(e)=>{var d=e.data;if(d&&d.cmd==='pcm'){" +
                  "var s=new Int16Array(d.buf);var n=s.length>>1;" +
                  "for(var i=0;i<n;i++){" +
                    /* overflow: drop the OLDEST samples (advance read), keeping
                       the freshest audio — realtime-correct after a stall */
                    "if(this.count>=this.cap){this.r=(this.r+1)%this.cap;this.count--;this.frac=0;this.drop++;if(this.fade===0){this.fromL=this.lastL;this.fromR=this.lastR;this.fade=128;this.recoveries++;}}" +
                    "this.L[this.w]=s[2*i]/32768;this.R[this.w]=s[2*i+1]/32768;" +
                    "this.w=(this.w+1)%this.cap;this.count++;}}" +
                  /* A pre-ready loss was already returned as a non-success
                   * from Push(), so this message only establishes the same
                   * continuity envelope. Do not add it to this.drop or C
                   * would observe the one loss twice. */
                  "else if(d&&d.cmd==='gap'){this.fromL=this.lastL;this.fromR=this.lastR;this.fade=128;this.recoveries++;}};}" +
              "process(inputs,outputs){var o=outputs[0];if(!o||!o[0])return true;" +
                "var L=o[0];var R=o[1]||o[0];var n=L.length;" +
                "for(var i=0;i<n;i++){" +
                  "if(this.count<2){L[i]=0;R[i]=0;this.under++;continue;}" +
                  "var i1=(this.r+1)%this.cap;var f=this.frac;" +
                  "var l=this.L[this.r]*(1-f)+this.L[i1]*f;var r=this.R[this.r]*(1-f)+this.R[i1]*f;" +
                  "if(this.fade>0){var t=(129-this.fade)/128;l=this.fromL*(1-t)+l*t;r=this.fromR*(1-t)+r*t;this.fade--;this.recoverySamples++;if(this.fade===0)this.completedRecoveries++;}" +
                  "L[i]=l;R[i]=r;this.lastL=l;this.lastR=r;" +
                  "this.frac+=this.ratio;" +
                  "while(this.frac>=1){this.frac-=1;this.r=(this.r+1)%this.cap;this.count--;}}" +
                /* stamp the ring depth with the AUDIO-thread context time at send */
                "if(++this.q>=3){this.q=0;this.port.postMessage({ring:this.count,under:this.under,dropped:this.drop,recoveries:this.recoveries,fade:this.fade,recoverySamples:this.recoverySamples,completedRecoveries:this.completedRecoveries,t:currentTime});}" +
                "return true;}}" +
            "registerProcessor('mdkr-ring',MDKRRing);";

        var url = URL.createObjectURL(new Blob([src], { type: 'application/javascript' }));
        ctx.audioWorklet.addModule(url).then(function () {
            try { URL.revokeObjectURL(url); } catch (e) {}
            if (A.closed) return;
            var node = new AudioWorkletNode(ctx, 'mdkr-ring', {
                numberOfInputs: 0, numberOfOutputs: 1,
                outputChannelCount: [channels],
                processorOptions: { srcRate: srcRate }
            });
            node.port.onmessage = function (e) {
                if (A.closed) return;
                A.statRing = e.data.ring;
                A.statTime = e.data.t;
                A.postedAtStat = A.posted;
                A.under = e.data.under;
                /* Worklet counters are cumulative. Keep a separate last-seen
                 * value so ConsumeDroppedFrames() may reset only the pending
                 * C-side delta without causing old losses to be reported on
                 * every later status packet. */
                var reportedDrops = Number(e.data.dropped) || 0;
                if (reportedDrops > A.droppedSeen) {
                    var newlyDropped = reportedDrops - A.droppedSeen;
                    A.droppedFrames += newlyDropped;
                    A.droppedTotal += newlyDropped;
                }
                A.droppedSeen = reportedDrops;
                A.recoveries = Number(e.data.recoveries) || 0;
                A.recoveryFrames = Number(e.data.fade) || 0;
                A.recoverySamples = Number(e.data.recoverySamples) || 0;
                A.completedRecoveries = Number(e.data.completedRecoveries) || 0;
            };
            node.connect(ctx.destination);
            A.node = node;
            A.ready = true;
            if (A.pendingDrops > 0) {
                node.port.postMessage({ cmd: 'gap', frames: A.pendingDrops });
                A.pendingDrops = 0;
            }
            for (var i = 0; i < A.pending.length; i++) {
                node.port.postMessage({ cmd: 'pcm', buf: A.pending[i] }, [A.pending[i]]);
            }
            A.pending = [];
        }).catch(function (err) {
            /* Module load failed after init already returned 1: the backend stays
             * COMMITTED but silent (push discards, occupancy reports empty) so the
             * C side never issues SDL calls against a device it never opened. */
            A.failed = true;
            if (console && console.error) console.error('[audio] AudioWorklet module load failed: ' + err);
        });

        /* Commit the backend ONLY now that every synchronous step above
         * succeeded (a throw before this leaves init returning 0). */
        Module.mdkrAudio = A;
        /* Mirror onto SDL2.audioContext so the shell's resume/close lifecycle
         * (gesture-resume, teardown) works unchanged. Safe: we skip
         * SDL_OpenAudioDevice on web, so SDL never sets it. */
        Module.SDL2 = Module.SDL2 || {};
        Module.SDL2.audioContext = ctx;
        return 1;
    } catch (e) {
        try { if (ctx) ctx.close(); } catch (e2) {}
        return 0;
    }
});

/* "Active" means COMMITTED (init returned 1, engine skipped the SDL device) —
 * deliberately NOT "&& !failed": once committed there is no SDL device to fall
 * back to, so a failed-but-committed backend stays active and discards silently. */
EM_JS(int, webAudioOutputActive, (void), {
    return Module.mdkrAudio ? 1 : 0;
});

EM_JS(int, webAudioOutputPush, (const void *buf, unsigned size), {
    var A = Module.mdkrAudio;
    if (!A) return -1;
    if (A.closed || A.failed) return -1;
    var frames = size >> 2;                       /* stereo s16 -> 4 bytes/frame */
    /* Copy out of the (growable) wasm heap into an owned, transferable buffer. */
    var view = HEAP16.subarray(buf >> 1, (buf >> 1) + (size >> 1));
    var copy = new Int16Array(view);
    if (A.ready && A.node) {
        A.node.port.postMessage({ cmd: 'pcm', buf: copy.buffer }, [copy.buffer]);
    } else if (A.pending.length < 64) {           /* buffer briefly until module loads */
        A.pending.push(copy.buffer);
    } else {
        A.pendingDrops += frames;
        A.pendingDroppedFrames += frames;
        return 1;                                 /* observable, crossfaded gap */
    }
    A.posted += frames;
    return 0;
});

EM_JS(unsigned, webAudioOutputConsumeDroppedFrames, (void), {
    var A = Module.mdkrAudio;
    if (!A || !A.droppedFrames) return 0;
    var dropped = A.droppedFrames >>> 0;
    A.droppedFrames = 0;
    return dropped;
});

EM_JS(unsigned, webAudioOutputQueuedBytes, (void), {
    var A = Module.mdkrAudio;
    if (!A || A.failed || A.closed) return 0;
    /* While the context is not running (iOS pre-gesture, interruptions,
     * app switches) nothing drains: report a full ring so the pump's
     * occupancy controller throttles to its floor instead of building a
     * backlog the player later hears as delay. On resume the near-empty
     * ring refills to the ~2-field target within a pump. */
    if (A.ctx && A.ctx.state !== 'running') {
        return Math.max(8192, (((A.srcRate * 2) / 5) | 0)) * 4;
    }
    var occ;
    if (A.statTime > 0) {
        var elapsed = A.ctx.currentTime - A.statTime;
        occ = A.statRing + (A.posted - A.postedAtStat) - elapsed * A.srcRate;
    } else {
        occ = A.posted;                           /* nothing has played yet */
    }
    if (occ < 0) occ = 0;
    return (occ | 0) * 4;
});

EM_JS(void, webAudioOutputResume, (void), {
    var A = Module.mdkrAudio;
    if (A && !A.closed && A.ctx && A.ctx.state !== 'running') {
        A.ctx.resume().catch(function () {});
    }
});

EM_ASYNC_JS(void, webAudioOutputShutdown, (void), {
    var A = Module.mdkrAudio;
    if (!A) return;
    if (A.shutdownComplete) return;
    A.closed = true;
    A.pending = [];
    if (A.node) {
        try { A.node.port.onmessage = null; } catch (e) {}
        try { A.node.port.close(); } catch (e) {}
        try { A.node.disconnect(); } catch (e) {}
        A.node = null;
    }
    if (A.ctx && A.ctx.state !== 'closed') {
        try { await A.ctx.close(); } catch (e) {}
    }
    if (Module.SDL2 && Module.SDL2.audioContext === A.ctx) {
        Module.SDL2.audioContext = null;
    }
    A.shutdownComplete = true;
});

#endif /* __EMSCRIPTEN__ */

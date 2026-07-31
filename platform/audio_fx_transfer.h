/* Bounds-safe planning for ALFx circular delay-line DMA transfers. */
#ifndef MGB64_AUDIO_FX_TRANSFER_H
#define MGB64_AUDIO_FX_TRANSFER_H

/* mixerLoadBuffer/mixerSaveBuffer round every transfer to eight bytes. Four
 * s16 samples guarantee a complete eight-byte tail at the logical boundary. */
#define PORT_AUDIO_FX_TAIL_SLACK_SAMPLES 4u

struct PortAudioFxTransferPlan
{
    unsigned int first_offset;
    unsigned int first_samples;
    unsigned int second_offset;
    unsigned int second_samples;
    unsigned int planned_samples;
    int          clamped;
};

int portAudioFxPlanTransfer(
    unsigned int                    logical_length,
    unsigned int                    allocated_samples,
    unsigned int                    current_offset,
    unsigned int                    requested_samples,
    struct PortAudioFxTransferPlan *out);

void         portAudioFxRecordGuardTrip(void);
unsigned int portAudioFxGuardTripCount(void);

#endif /* MGB64_AUDIO_FX_TRANSFER_H */

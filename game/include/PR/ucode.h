/*
 * ucode.h — clean-room RSP microcode symbol declarations.
 *
 * Declares the extern symbols marking the start/end of each RSP microcode
 * object's text and data (boot, the various Fast3D/F3DEX/F3DLX/L3DEX
 * graphics microcodes, and the basic audio microcode), plus the stack and
 * IMEM/DMEM sizing constants those microcodes require. This is a
 * declaration-only compatibility surface; the actual microcode objects are
 * supplied elsewhere and linked against these symbols.
 */

#ifndef _UCODE_H_
#define	_UCODE_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/ultratypes.h>

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/**************************************************************************
 *
 * Macro definitions
 *
 */

/*
 * Recommended size of the SP DRAM stack used by the graphics microcode.
 * The matrix stack is its main consumer, so this must be at least
 * 10 * 64 bytes.
 */
#define	SP_DRAM_STACK_SIZE8	(1024)
#define	SP_DRAM_STACK_SIZE64	(SP_DRAM_STACK_SIZE8 >> 3)

/*
 * Size of IMEM, matching the size of the graphics microcode (other
 * microcode objects may be smaller). Applications use this value to tell
 * the OS how much microcode to load.
 */
#define SP_UCODE_SIZE           4096

/*
 * Half the size of DMEM — the largest amount of initialized DMEM data that
 * any microcode task needs at startup. Depends on every task microcode, so
 * it is fixed for a given microcode release.
 */
#define SP_UCODE_DATA_SIZE      2048
   

/**************************************************************************
 *
 * Extern variables
 *
 */

/*
 * These symbols come from running the microcode objects through the ELF
 * conversion step used when building a ROM; each pair marks the location
 * and size of one SP microcode object. They're loaded here as part of the
 * code segment, though other placements would work just as well.
 */

/* standard boot ucode: */
extern long long int	rspbootTextStart[], rspbootTextEnd[];

/* standard 3D ucode: */
extern long long int	gspFast3DTextStart[], gspFast3DTextEnd[];
extern long long int	gspFast3DDataStart[], gspFast3DDataEnd[];

/* 3D ucode with output to DRAM: */
extern long long int	gspFast3D_dramTextStart[], gspFast3D_dramTextEnd[];
extern long long int	gspFast3D_dramDataStart[], gspFast3D_dramDataEnd[];

/* 3D ucode with output through DRAM FIFO to RDP: */
extern long long int	gspFast3D_fifoTextStart[], gspFast3D_fifoTextEnd[];
extern long long int	gspFast3D_fifoDataStart[], gspFast3D_fifoDataEnd[];

/* 3D ucode without nearclip: */
extern long long int	gspF3DNoNTextStart[], gspF3DNoNTextEnd[];
extern long long int	gspF3DNoNDataStart[], gspF3DNoNDataEnd[];

/* 3D ucode without nearclip with output to DRAM: */
extern long long int	gspF3DNoN_dramTextStart[];
extern long long int	gspF3DNoN_dramTextEnd[];
extern long long int	gspF3DNoN_dramDataStart[];
extern long long int	gspF3DNoN_dramDataEnd[];

/* 3D ucode without nearclip with output through DRAM FIFO to RDP: */
extern long long int	gspF3DNoN_fifoTextStart[];
extern long long int	gspF3DNoN_fifoTextEnd[];
extern long long int	gspF3DNoN_fifoDataStart[];
extern long long int	gspF3DNoN_fifoDataEnd[];

/* 3D line ucode: */
extern long long int	gspLine3DTextStart[], gspLine3DTextEnd[];
extern long long int	gspLine3DDataStart[], gspLine3DDataEnd[];

/* 3D line ucode with output to DRAM: */
extern long long int	gspLine3D_dramTextStart[], gspLine3D_dramTextEnd[];
extern long long int	gspLine3D_dramDataStart[], gspLine3D_dramDataEnd[];

/* 3D line ucode with output through DRAM FIFO to RDP: */
extern long long int	gspLine3D_fifoTextStart[], gspLine3D_fifoTextEnd[];
extern long long int	gspLine3D_fifoDataStart[], gspLine3D_fifoDataEnd[];

/* 2D sprite ucode: */
extern long long int	gspSprite2DTextStart[], gspSprite2DTextEnd[];
extern long long int	gspSprite2DDataStart[], gspSprite2DDataEnd[];

/* 2D sprite ucode with output to DRAM: */
extern long long int	gspSprite2D_dramTextStart[], gspSprite2D_dramTextEnd[];
extern long long int	gspSprite2D_dramDataStart[], gspSprite2D_dramDataEnd[];

/* 2D sprite ucode with output through DRAM FIFO to RDP: */
extern long long int	gspSprite2D_fifoTextStart[], gspSprite2D_fifoTextEnd[];
extern long long int	gspSprite2D_fifoDataStart[], gspSprite2D_fifoDataEnd[];

/* basic audio ucode: */
extern long long int 	aspMainTextStart[], aspMainTextEnd[];
extern long long int 	aspMainDataStart[], aspMainDataEnd[];

/*========== F3DEX/F3DLX/F3DLP/L3DEX ==========*/
/* FIFO version only */
extern long long int  gspF3DEX_fifoTextStart[],     gspF3DEX_fifoTextEnd[];
extern long long int  gspF3DEX_fifoDataStart[],     gspF3DEX_fifoDataEnd[];
extern long long int  gspF3DEX_NoN_fifoTextStart[], gspF3DEX_NoN_fifoTextEnd[];
extern long long int  gspF3DEX_NoN_fifoDataStart[], gspF3DEX_NoN_fifoDataEnd[];

extern long long int  gspF3DLX_fifoTextStart[],     gspF3DLX_fifoTextEnd[];
extern long long int  gspF3DLX_fifoDataStart[],     gspF3DLX_fifoDataEnd[];
extern long long int  gspF3DLX_NoN_fifoTextStart[], gspF3DLX_NoN_fifoTextEnd[];
extern long long int  gspF3DLX_NoN_fifoDataStart[], gspF3DLX_NoN_fifoDataEnd[];
extern long long int  gspF3DLX_Rej_fifoTextStart[], gspF3DLX_Rej_fifoTextEnd[];
extern long long int  gspF3DLX_Rej_fifoDataStart[], gspF3DLX_Rej_fifoDataEnd[];

extern long long int  gspF3DLP_Rej_fifoTextStart[], gspF3DLP_Rej_fifoTextEnd[];
extern long long int  gspF3DLP_Rej_fifoDataStart[], gspF3DLP_Rej_fifoDataEnd[];
extern long long int  gspL3DEX_fifoTextStart[],     gspL3DEX_fifoTextEnd[];
extern long long int  gspL3DEX_fifoDataStart[],     gspL3DEX_fifoDataEnd[];

/*========== F3DEX2/F3DLX2/F3DLP2/L3DEX2 ==========*/
/* FIFO version */
extern long long int gspF3DEX2_fifoTextStart[],    gspF3DEX2_fifoTextEnd[];
extern long long int gspF3DEX2_fifoDataStart[],    gspF3DEX2_fifoDataEnd[];
extern long long int gspF3DEX2_NoN_fifoTextStart[],gspF3DEX2_NoN_fifoTextEnd[];
extern long long int gspF3DEX2_NoN_fifoDataStart[],gspF3DEX2_NoN_fifoDataEnd[];
extern long long int gspF3DEX2_Rej_fifoTextStart[],gspF3DEX2_Rej_fifoTextEnd[];
extern long long int gspF3DEX2_Rej_fifoDataStart[],gspF3DEX2_Rej_fifoDataEnd[];
extern long long int gspF3DLX2_Rej_fifoTextStart[],gspF3DLX2_Rej_fifoTextEnd[];
extern long long int gspF3DLX2_Rej_fifoDataStart[],gspF3DLX2_Rej_fifoDataEnd[];
extern long long int gspL3DEX2_fifoTextStart[],    gspL3DEX2_fifoTextEnd[];
extern long long int gspL3DEX2_fifoDataStart[],    gspL3DEX2_fifoDataEnd[];

/* XBUS version */
extern long long int gspF3DEX2_xbusTextStart[],    gspF3DEX2_xbusTextEnd[];
extern long long int gspF3DEX2_xbusDataStart[],    gspF3DEX2_xbusDataEnd[];
extern long long int gspF3DEX2_NoN_xbusTextStart[],gspF3DEX2_NoN_xbusTextEnd[];
extern long long int gspF3DEX2_NoN_xbusDataStart[],gspF3DEX2_NoN_xbusDataEnd[];
extern long long int gspF3DEX2_Rej_xbusTextStart[],gspF3DEX2_Rej_xbusTextEnd[];
extern long long int gspF3DEX2_Rej_xbusDataStart[],gspF3DEX2_Rej_xbusDataEnd[];
extern long long int gspF3DLX2_Rej_xbusTextStart[],gspF3DLX2_Rej_xbusTextEnd[];
extern long long int gspF3DLX2_Rej_xbusDataStart[],gspF3DLX2_Rej_xbusDataEnd[];
extern long long int gspL3DEX2_xbusTextStart[],    gspL3DEX2_xbusTextEnd[];
extern long long int gspL3DEX2_xbusDataStart[],    gspL3DEX2_xbusDataEnd[];

/**************************************************************************
 *
 * Function prototypes
 *
 */

#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_UCODE_H */

/*
Copyright (c) 2017, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef _SOFTWARE_JTAG_HANDLER_H_
#define _SOFTWARE_JTAG_HANDLER_H_

#include "asd_common.h"

// contains common ioctl command numbers for accessing jtag driver and structs
// JtagStates enum is defined here
#include "jtag_drv.h"

#define MAXPADSIZE 512

typedef enum {
    JTAGDriverState_Master = 0,
    JTAGDriverState_Slave
} JTAGDriverState;

typedef enum {
    JTAGPaddingTypes_IRPre,
    JTAGPaddingTypes_IRPost,
    JTAGPaddingTypes_DRPre,
    JTAGPaddingTypes_DRPost
} JTAGPaddingTypes;

typedef enum {
    JTAGScanState_Done = 0,
    JTAGScanState_Run
} JTAGScanState;

typedef struct JTAGShiftPadding {
    int drPre;
    int drPost;
    int irPre;
    int irPost;
} JTAGShiftPadding;

typedef struct JTAG_Handler {
    JtagStates tap_state;
    JTAGShiftPadding shift_padding;
    unsigned char padDataOne[MAXPADSIZE];
    unsigned char padDataZero[MAXPADSIZE];
    JTAGScanState scan_state;
    int JTAG_driver_handle;
} JTAG_Handler;

#ifdef __cplusplus
extern "C" {
#endif

JTAG_Handler* SoftwareJTAGHandler();
STATUS JTAG_initialize(JTAG_Handler* state);
STATUS JTAG_deinitialize(JTAG_Handler* state);
STATUS JTAG_set_padding(JTAG_Handler* state, const JTAGPaddingTypes padding, const int value);
STATUS JTAG_tap_reset(JTAG_Handler* state);
STATUS JTAG_set_tap_state(JTAG_Handler* state, JtagStates tap_state);
STATUS JTAG_get_tap_state(JTAG_Handler* state, JtagStates* tap_state);
STATUS JTAG_shift(JTAG_Handler* state, unsigned int number_of_bits,
                  unsigned int input_bytes, unsigned char* input,
                  unsigned int output_bytes, unsigned char* output,
                  JtagStates end_tap_state);
STATUS JTAG_wait_cycles(JTAG_Handler* state, unsigned int number_of_cycles);
STATUS JTAG_set_clock_frequency(JTAG_Handler* state, unsigned int frequency);

#ifdef __cplusplus
}
#endif

#endif  // _SOFTWARE_JTAG_HANDLER_H_

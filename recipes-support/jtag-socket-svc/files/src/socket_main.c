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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <stdbool.h>
/* #include <safe_lib.h> */
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>

#include "SoftwareJTAGHandler.h"
#include "target_handler.h"
#include "logging.h"
#include "asd_common.h"

#define EVENT_FD_INDEX				0
#define HOST_FD_INDEX				1
#define CLIENT_FD_INDEX				2
#define MAX_DATA_SIZE				TDO_DATA_SIZE
#define NUM_IN_FLIGHT_BUFFERS_TO_USE		20
#define DEFAULT_PORT				5123
#define NUM_IN_FLIGHT_MESSAGES_SUPPORTED_CMD	3
#define OBTAIN_DOWNSTREAM_VERSION_CMD		5

// Two simple rules for the version string are:
// 1. less than 265 in length (or it will be truncated in the plugin)
// 2. no dashes, as they are used later up the sw stack between components.
static char asd_version[] = "ASD_BMC_v0.92";

typedef enum {
	CLOSE_CLIENT_EVENT = 1
} InternalEventTypes;

typedef enum {
	SOCKET_READ_STATE_INITIAL = 0,
	SOCKET_READ_STATE_HEADER,
	SOCKET_READ_STATE_BUFFER,
} SocketReadState;

static int comm_fd = 0, host_fd = 0, event_fd = 0;
static uint8_t prdy_timeout = 1;

static struct spi_message out_msg;
char* send_buffer;
static pthread_mutex_t send_buffer_mutex;

static JTAG_Handler* jtag_handler = NULL;
static Target_Control_Handle* target_control_handle = NULL;

bool prnt_irdr = false, prnt_net = false,
     prnt_jtagMsg = false, prnt_Debug = false,
     write_to_syslog = false;
int port_number = DEFAULT_PORT;


void showUsage(char **argv)
{
	fprintf(stderr, "Usage: %s [option(s)]\n", argv[0]);
	fprintf(stderr, "  -l <number>   Print verbose debug data:\n");
	fprintf(stderr, "     1          IR/DR transactions\n");
	fprintf(stderr, "     2          network transactions\n");
	fprintf(stderr, "     3          JTAG MSG from plugin\n");
	fprintf(stderr, "     4          All log types\n");
	fprintf(stderr, "  -p <number>   Port number (default=%d)\n", DEFAULT_PORT);
	fprintf(stderr, "  -s            Route log messages to the system log\n\n");
}

void process_command_line(int argc, char **argv)
{
	int c = 0;
	while ((c = getopt(argc, argv, "l:p:s")) != -1) {
		switch (c) {
		case 'l': {
			int i = atoi(optarg);
			ASD_LogType type = LogType_None;
			if (i > LogType_MIN && i < LogType_MAX)
				type = (ASD_LogType)i;
			else
				fprintf(stderr, "invalid log type %d\n", i);

			ASD_initialize_log_settings(type);
			break;
		}
		case 'p': {
			int port = atoi(optarg);
			fprintf(stderr, "Setting Port: %d\n", port);
			port_number = port;
			break;
		}
		case 's':
			write_to_syslog = true;
			break;
		default:  // h, ?, and other
			showUsage(argv);
			exit(EXIT_SUCCESS);
		}
	}
	if (optind < argc) {
		fprintf(stderr, "invalid non-option argument(s)\n");
		showUsage(argv);
		exit(EXIT_SUCCESS);
	}
}

void print_version()
{
#ifndef ADTM_VERSION
#define ADTM_VERSION __DATE__
#endif
	fprintf(stderr, "version: " ADTM_VERSION "\n");
}

void exitAll() {
	if (host_fd != 0)
		close(host_fd);
	if (comm_fd != 0)
		close(comm_fd);
	if (out_msg.buffer)
		free(out_msg.buffer);
	if (send_buffer)
		free(send_buffer);
	if (jtag_handler) {
		free(jtag_handler);
		jtag_handler = NULL;
	}
	if (target_control_handle) {
		free(target_control_handle);
		target_control_handle = NULL;
	}
}

void send_error_message(int comm_fd, struct spi_message *input_message,
			ASDError cmd_stat)
{
	ssize_t cnt = 0;
	struct spi_message error_message = {{0}};  // initialize the struct

	if (comm_fd < 0)
		return;

	memcpy(&error_message.header,
		&(input_message->header), sizeof(struct message_header));

	error_message.header.cmd_stat = cmd_stat;
	cnt = send(comm_fd, &error_message.header, sizeof(struct message_header), 0);
	if (cnt != sizeof(error_message.header)) {
		ASD_log(LogType_Error, "Failed to send an error message to client: %d", cnt);
	}
}

int get_message_size(struct spi_message *s_message)
{
	int result = (s_message->header.size_lsb & 0xFF) |
		((s_message->header.size_msb & 0x1F) << 8);
	if (result > MAX_DATA_SIZE)
		result = -1;
	return result;
}

STATUS write_event_config(const uint8_t data_byte)
{
	STATUS status = ST_OK;
	// If bit 7 is set, then we will be enabling an event, otherwise disabling
	bool enable = (data_byte >> 7) == 1;
	// Bits 0 through 6 are an unsigned int indicating the event config register
	uint8_t event_cfg_raw = data_byte & WRITE_CFG_MASK;
	if (event_cfg_raw > WRITE_CONFIG_MIN
			&& event_cfg_raw < WRITE_CONFIG_MAX) {
		WriteConfig event_cfg = (WriteConfig)event_cfg_raw;
		status = target_write_event_config(target_control_handle, event_cfg, enable);
	} else {
		// We will not return an error for this, as unsupported may be okay
		ASD_log(LogType_Error, "Invalid or unsupported write event config register: "
				"0x%02x", event_cfg_raw);
	}
	return status;
}

STATUS write_cfg(const writeCfg cmd, const unsigned char *data, int *cnt)
{
	if (cmd == JTAG_FREQ) {
		// JTAG_FREQ (Index 1, Size: 1 Byte)
		//  Bit 7:6 – Prescale Value (b'00 – 1, b'01 – 2, b'10 – 4, b'11 – 8)
		//  Bit 5:0 – Divisor (1-64, 64 is expressed as value 0)
		//  The TAP clock frequency is determined by dividing the system clock
		//  of the TAP Master first through the prescale value (1,2,4,8) and
		//  then through the divisor (1-64).
		// e.g. system clock/(prescale * divisor)
		int prescaleVal = 0, divisorVal = 0, tCLK = 0;
		int baseFreq = 12000000;
		(*cnt)++;
		// PCLK is 12MHz on WFP targets
		prescaleVal = data[0] >> 5;
		divisorVal = data[0] & 0x1f;

		if (prescaleVal == 0) {
			prescaleVal = 1;
		} else if (prescaleVal == 1) {
			prescaleVal = 2;
		} else if (prescaleVal == 2) {
			prescaleVal = 4;
		} else if (prescaleVal == 3) {
			prescaleVal = 8;
		} else {
			prescaleVal = 1;
		}

		if (divisorVal == 0) {
			divisorVal = 64;
		}

		tCLK = (baseFreq/(prescaleVal*divisorVal));

		if (JTAG_set_clock_frequency(jtag_handler, tCLK) != ST_OK) {
			ASD_log(LogType_Error, "Unable to set the TAP frequency!");
			return ST_ERR;
		}
		return ST_OK;
	} else if (cmd == DR_PREFIX) {
		// DR Postfix (A.K.A. DR Prefix in At-Scale Debug Arch. Spec.)
		// set drPaddingNearTDI 1 byte of data
		(*cnt)++;
		ASD_log(LogType_IRDR, "Setting DRPost padding to %d", data[0]);
		if (JTAG_set_padding(jtag_handler, JTAGPaddingTypes_DRPost, data[0]) != ST_OK) {
			ASD_log(LogType_Error, "failed to set DRPost padding");
			return ST_ERR;
		}
		return ST_OK;
	} else if (cmd == DR_POSTFIX) {
		// DR preFix (A.K.A. DR Postfix in At-Scale Debug Arch. Spec.)
		// drPaddingNearTDO 1 byte of data
		(*cnt)++;
		ASD_log(LogType_IRDR, "Setting DRPre padding to %d", data[0]);
		if (JTAG_set_padding(jtag_handler, JTAGPaddingTypes_DRPre, data[0]) != ST_OK) {
			ASD_log(LogType_Error, "failed to set DRPre padding");
			return ST_ERR;
		}
		return ST_OK;
	} else if (cmd == IR_PREFIX) {
		// IR Postfix (A.K.A. IR Prefix in At-Scale Debug Arch. Spec.)
		// irPaddingNearTDI 2 bytes of data
		(*cnt) += 2;
		ASD_log(LogType_IRDR, "Setting IRPost padding to %d", (data[1] << 8) | data[0]);
		if (JTAG_set_padding(jtag_handler, JTAGPaddingTypes_IRPost, (data[1] << 8) | data[0]) != ST_OK) {
			ASD_log(LogType_Error, "failed to set IRPost padding");
			return ST_ERR;
		}
		return ST_OK;
	} else if (cmd == IR_POSTFIX) {
		// IR Prefix (A.K.A. IR Postfix in At-Scale Debug Arch. Spec.)
		// irPaddingNearTDO 2 bytes of data
		(*cnt) += 2;
		ASD_log(LogType_IRDR, "Setting IRPre padding to %d", (data[1] << 8) | data[0]);
		if (JTAG_set_padding(jtag_handler, JTAGPaddingTypes_IRPre, (data[1] << 8) | data[0]) != ST_OK) {
			ASD_log(LogType_Error, "failed to set IRPre padding");
			return ST_ERR;
		}
		return ST_OK;
	} else if (cmd == PRDY_TIMEOUT) {
		// PRDY timeout
		// 1 bytes of data
		ASD_log(LogType_Debug, "PRDY Timeout config set to %d", data[0]);
		prdy_timeout = data[0];
		(*cnt)++;
		return ST_OK;
	} else {
		ASD_log(LogType_Error, "ERROR: WriteCFG encountered unrecognized command (%d)", cmd);
		return ST_ERR;
	}
	return ST_ERR;
}

STATUS read_status(const ReadType index, uint8_t pin,
		const unsigned char *return_buffer,
		const int return_buffer_size, int *bytes_written)
{
	STATUS status = ST_OK;
	*bytes_written = 0;
	bool pinAsserted = false;
	if (return_buffer_size < 2) {
		ASD_log(LogType_Error, "Failed to process READ_STATUS. Response buffer already full");
		status = ST_ERR;
	}
	if (status == ST_OK) {
		status = target_read(target_control_handle, index, pin, &pinAsserted);
	}
	if (status == ST_OK) {
		out_msg.buffer[(*bytes_written)++] = READ_STATUS_MIN + index;
		out_msg.buffer[*bytes_written] = pin;
		if (pinAsserted)
			out_msg.buffer[*bytes_written] |= 1 << 7;  // set 7th bit
		(*bytes_written)++;
	}
	return status;
}

STATUS send_out_msg_on_socket(struct spi_message *message)
{
	int send_buffer_size = 0;
	int cnt = 0;
	int size = get_message_size(message);

	if (comm_fd < 0 || message == NULL)
		return ST_ERR;

	if (size == -1) {
		ASD_log(LogType_Error, "Failed to send message because get message size failed.");
		return ST_ERR;
	}

	if (prnt_net) {
		ASD_log(LogType_NETWORK,
				"Response header:  origin_id: 0x%02x\n"
				"    reserved: 0x%02x    enc_bit: 0x%02x\n"
				"    type: 0x%02x        size_lsb: 0x%02x\n"
				"    size_msb: 0x%02x    tag: 0x%02x\n"
				"    cmd_stat: 0x%02x",
				message->header.origin_id, message->header.reserved,
				message->header.enc_bit, message->header.type,
				message->header.size_lsb, message->header.size_msb,
				message->header.tag, message->header.cmd_stat);
		ASD_log(LogType_NETWORK, "Response Buffer size: %d", size);
		ASD_log_buffer(LogType_NETWORK, message->buffer, size, "NetRsp");
	}

	send_buffer_size = sizeof(struct message_header) + size;

	pthread_mutex_lock(&send_buffer_mutex);

	memcpy(send_buffer,
		(unsigned char*)&message->header, sizeof(message->header));
	memcpy(send_buffer + sizeof(message->header),
		message->buffer, size);

	cnt = send(comm_fd, send_buffer, send_buffer_size, 0);
	pthread_mutex_unlock(&send_buffer_mutex);
	if (cnt != send_buffer_size) {
		ASD_log(LogType_Error, "Failed to write message buffer to the socket: %d", cnt);
		return ST_ERR;
	}

	return ST_OK;
}

static void get_scan_length(const char cmd, uint8_t *num_of_bits, uint8_t *num_of_bytes)
{
	*num_of_bits = (cmd & SCAN_LENGTH_MASK);
	*num_of_bytes = (*num_of_bits + 7) / 8;

	if (*num_of_bits == 0) {
		// this is 64bits
		*num_of_bytes = 8;
		*num_of_bits = 64;
	}
}

STATUS process_jtag_message(struct spi_message *s_message)
{
	int cnt = 0, response_cnt = 0;
	JtagStates end_state;
	STATUS status = ST_OK;
	int size = get_message_size(s_message);
	if (size == -1) {
		ASD_log(LogType_Error, "Failed to process jtag message because "
				"get message size failed.");
		return ST_ERR;
	}

	memset(&out_msg.header, 0, sizeof(struct message_header));
	memset(out_msg.buffer, 0, MAX_DATA_SIZE);

	if (prnt_jtagMsg) {
		ASD_log(LogType_JTAG, "NetReq size: %d", size);
		ASD_log_buffer(LogType_JTAG, s_message->buffer, size, "NetReq");
	}

	while (cnt < size) {
		char cmd = s_message->buffer[cnt];
		cnt++;  // increment 1 to account for the command byte
		if (cmd == WRITE_EVENT_CONFIG) {
			status = write_event_config(s_message->buffer[cnt]);
			cnt++;  // increment for the data byte
		} else if (cmd >= WRITE_CFG_MIN && cmd <= WRITE_CFG_MAX) {
			// the number of bytes to process vary by the config, so cnt is
			// incremented inside of write_cfg() to account for data bytes.
			status = write_cfg((writeCfg)cmd, &s_message->buffer[cnt], &cnt);
		} else if (cmd == WRITE_PINS) {
			uint8_t data = (int)s_message->buffer[cnt];
			bool assert = (data >> 7) == 1;
			Pin pin = PIN_MIN;
			uint8_t index = data & WRITE_PIN_MASK;
			if (index > PIN_MIN && index < PIN_MAX) {
				pin = (Pin)index;
				status = target_write(target_control_handle, pin, assert);
				cnt++;  // increment for the data byte
			} else {
				ASD_log(LogType_Error, "Unexpected WRITE_PINS index: 0x%02x", index);
				status = ST_ERR;
			}
		} else if (cmd >= READ_STATUS_MIN && cmd <= READ_STATUS_MAX) {
			int bytes_written = 0;
			ReadType readStatusTypeIndex;
			uint8_t index = (cmd & READ_STATUS_MASK);
			uint8_t pin = (s_message->buffer[cnt] & READ_STATUS_PIN_MASK);
			if (index > READ_TYPE_MIN && index < READ_TYPE_MAX)
				readStatusTypeIndex = (ReadType)index;
			else {
				ASD_log(LogType_Error,  "Unexpected READ_STATUS index: 0x%02x", index);
				status = ST_ERR;
			}
			cnt++;  // increment for the data byte
			if (status == ST_OK) {
				status = read_status(readStatusTypeIndex, pin,
						&out_msg.buffer[response_cnt],
						MAX_DATA_SIZE - response_cnt, &bytes_written);
				response_cnt += bytes_written;
			}
		} else if (cmd == WAIT_CYCLES_TCK_DISABLE ||
				cmd == WAIT_CYCLES_TCK_ENABLE) {
			unsigned int number_of_cycles = s_message->buffer[cnt];
			if (cmd == WAIT_CYCLES_TCK_ENABLE) {
				status = JTAG_wait_cycles(jtag_handler, number_of_cycles);
			} else {
				ASD_log(LogType_Debug, "Disabled wait cycle of 0x%x", number_of_cycles);
			}
			cnt++;  // account for the number of cycles data byte
		} else if (cmd == WAIT_PRDY) {
			status = target_wait_PRDY(target_control_handle, prdy_timeout);
		} else if (cmd == CLEAR_TIMEOUT) {
			// Command not yet implemented. This command does not apply to JTAG
			// so we will likely not implement it.
			ASD_log(LogType_Debug, "Clear Timeout command not yet implemented");
		} else if (cmd == TAP_RESET) {
			status = JTAG_tap_reset(jtag_handler);
		} else if (cmd >= TAP_STATE_MIN && cmd <= TAP_STATE_MAX) {
			status = JTAG_set_tap_state(jtag_handler, (JtagStates)(cmd & TAP_STATE_MASK));
		} else if (cmd >= WRITE_SCAN_MIN && cmd <= WRITE_SCAN_MAX) {
			uint8_t num_of_bits = 0;
			uint8_t num_of_bytes = 0;

			if (cmd == WRITE_SCAN_MAX) {
				num_of_bits = (uint8_t)s_message->buffer[cnt++];
				num_of_bytes = (num_of_bits + 7) / 8;
			} else
				get_scan_length(cmd, &num_of_bits, &num_of_bytes);

			status = JTAG_get_tap_state(jtag_handler, &end_state);
			if (status == ST_OK) {
				if ((cnt + num_of_bytes) < size) {
					char next_cmd = s_message->buffer[cnt + num_of_bytes];
					if (next_cmd >= TAP_STATE_MIN &&
							next_cmd <= TAP_STATE_MAX) {
						// Next command is a tap state command
						end_state = (JtagStates)(next_cmd & TAP_STATE_MASK);
					} else if (next_cmd < WRITE_SCAN_MIN ||
							next_cmd > WRITE_SCAN_MAX) {
						ASD_log(LogType_Error, "Unexpected sequence during write scan: 0x%02x", next_cmd);
						return ST_ERR;
					}
				}
				/* status = JTAG_shift(jtag_handler, num_of_bits, MAX_DATA_SIZE - cnt,
						(unsigned char*)&(s_message->buffer[cnt]),
						0, NULL, end_state);*/
				status = JTAG_shift(jtag_handler, num_of_bits, num_of_bytes,
						(unsigned char*)&(s_message->buffer[cnt]),
						0, NULL, end_state);
				cnt += num_of_bytes;  // Increment by the number of bytes written
				cnt ++;
			}
		} else if (cmd >= READ_SCAN_MIN && cmd <= READ_SCAN_MAX) {
			unsigned char dead_beef[8] = {0x0d, 0xf0, 0xd4, 0xba, 0xef, 0xbe, 0xad, 0xde};
			uint8_t num_of_bits = 0;
			uint8_t num_of_bytes = 0;
			get_scan_length(cmd, &num_of_bits, &num_of_bytes);
			status = JTAG_get_tap_state(jtag_handler, &end_state);
			if (status == ST_OK) {
				if (response_cnt + sizeof(char) + num_of_bytes > MAX_DATA_SIZE) {
					ASD_log(LogType_Error, "Failed to process READ_SCAN. "
							"Response buffer already full");
					status = ST_ERR;
				} else {
					out_msg.buffer[response_cnt++] = cmd;
					if (cnt < size) {
						char next_cmd = s_message->buffer[cnt];
						if (next_cmd >= TAP_STATE_MIN &&
								next_cmd <= TAP_STATE_MAX) {
							// Next command is a tap state command
							end_state = (JtagStates)(next_cmd & TAP_STATE_MASK);
						} else if (next_cmd < READ_SCAN_MIN ||
								next_cmd > READ_SCAN_MAX) {
							ASD_log(LogType_Error, "Unexpected sequence during read scan: 0x%02x", next_cmd);
							status = ST_ERR;
						}
					}
					if (status == ST_OK) {
						/*status = JTAG_shift(jtag_handler, num_of_bits, 0, NULL,
								MAX_DATA_SIZE-response_cnt,
								(unsigned char*)&(out_msg.buffer[response_cnt]),
								end_state);*/
						status = JTAG_shift(jtag_handler, num_of_bits, sizeof(dead_beef), dead_beef,
								MAX_DATA_SIZE - response_cnt,
								(unsigned char*)&(out_msg.buffer[response_cnt]),
								end_state);
						response_cnt += num_of_bytes;
						//cnt += num_of_bytes; // For READ_SCAN case, there should be no data following
						cnt ++;
					}
				}
			}
		} else if (cmd >= READ_WRITE_SCAN_MIN && cmd <= READ_WRITE_SCAN_MAX) {
			uint8_t num_of_bits = 0;
			uint8_t num_of_bytes = 0;

			if (cmd == READ_WRITE_SCAN_MAX) {
				num_of_bits = (uint8_t)s_message->buffer[cnt++];
				num_of_bytes = (num_of_bits + 7) / 8;
			} else
				get_scan_length(cmd, &num_of_bits, &num_of_bytes);

			status = JTAG_get_tap_state(jtag_handler, &end_state);
			if (status == ST_OK) {
				if (response_cnt + sizeof(char) + num_of_bytes > MAX_DATA_SIZE) {
					ASD_log(LogType_Error, "Failed to process READ_WRITE_SCAN. "
							"Response buffer already full");
					status = ST_ERR;
				} else {
					out_msg.buffer[response_cnt++] = cmd;
					if ((cnt + num_of_bytes) < size) {
						char next_cmd = s_message->buffer[cnt + num_of_bytes];
						if (next_cmd >= TAP_STATE_MIN &&
								next_cmd <= TAP_STATE_MAX) {
							// Next command is a tap state command
							end_state = (JtagStates)(next_cmd & TAP_STATE_MASK);
						} else if (next_cmd < READ_WRITE_SCAN_MIN ||
								next_cmd > READ_WRITE_SCAN_MAX) {
							ASD_log(LogType_Error, "Unexpected sequence during read/write scan: 0x%02x",
									next_cmd);
							status = ST_ERR;
						}
					}
					if (status == ST_OK) {
						/*status = JTAG_shift(jtag_handler, num_of_bits, MAX_DATA_SIZE-cnt,
								(unsigned char*)&(s_message->buffer[cnt]),
								MAX_DATA_SIZE-response_cnt,
								(unsigned char*)&(out_msg.buffer[response_cnt]),
								end_state);*/
						status = JTAG_shift(jtag_handler, num_of_bits, num_of_bytes,
								(unsigned char*)&(s_message->buffer[cnt]),
								MAX_DATA_SIZE - response_cnt,
								(unsigned char*)&(out_msg.buffer[response_cnt]),
								end_state);
						response_cnt += num_of_bytes; // response bytes are the same as we want to write?
						cnt += num_of_bytes; // Increment by the number of bytes written
						cnt ++;
					}
				}
			}
		} else {
			// Unknown Command
			ASD_log(LogType_Error, "Encountered unknown command 0x%02x", (int)cmd);
			status = ST_ERR;
		}

		if (status != ST_OK) {
			ASD_log(LogType_Error, "Failed to process command 0x%02x", (int)cmd);
			send_error_message(comm_fd, s_message, ASD_UNKNOWN_ERROR);
			break;
		}
	}

	if (status == ST_OK) {
		memcpy(&out_msg.header,
			&s_message->header, sizeof(struct message_header));

		out_msg.header.size_lsb = response_cnt & 0xFF;
		out_msg.header.size_msb = (response_cnt >> 8) & 0x1F;
		out_msg.header.cmd_stat = ASD_SUCCESS;

		status = send_out_msg_on_socket(&out_msg);
		if (status != ST_OK) {
			ASD_log(LogType_Error, "Failed to send message back on the socket");
		}
	}

	return status;
}

void process_message(int comm_fd, struct spi_message *s_message)
{
	if ((s_message->header.type != JTAG_TYPE)
			|| (s_message->header.cmd_stat != 0 &&
				s_message->header.cmd_stat != 0x80 &&
				s_message->header.cmd_stat != 1)) {
		ASD_log(LogType_Error, "Received the message that is not supported by this host\n"
				"Message must be in the format:\n"
				"Type = 1 & cmd_stat=0 or 1 or 0x80\n"
				"I got Type=%x, cmd_stat=%x",
				s_message->header.type, s_message->header.cmd_stat);
		send_error_message(comm_fd, s_message, ASD_UNKNOWN_ERROR);
		return;
	}

	if (process_jtag_message(s_message) != ST_OK) {
		ASD_log(LogType_Error, "Failed to process JTAG message");
		send_error_message(comm_fd, s_message, ASD_UNKNOWN_ERROR);
		return;
	}
}

static STATUS sendPinEvent(ASD_EVENT value)
{
	STATUS result = ST_OK;
	struct spi_message target_handler_msg = {{0}};
	target_handler_msg.buffer = (unsigned char*)malloc(1);
	if (!target_handler_msg.buffer) {
		ASD_log(LogType_Error, "Failed to allocate pin control event message buffer.");
		result = ST_ERR;
	} else {
		target_handler_msg.header.size_lsb = 1;
		target_handler_msg.header.size_msb = 0;
		target_handler_msg.header.type = JTAG_TYPE;
		target_handler_msg.header.tag = BROADCAST_MESSAGE_ORIGIN_ID;
		target_handler_msg.header.origin_id = BROADCAST_MESSAGE_ORIGIN_ID;
		target_handler_msg.buffer[0] = (value & 0xFF);

		if (send_out_msg_on_socket(&target_handler_msg) != ST_OK) {
			ASD_log(LogType_Error, "Failed to send pin control event to client.");
			result = ST_ERR;
		}
		free(target_handler_msg.buffer);
	}
	return result;
}

static STATUS TargetHandlerCallback(eventTypes event, ASD_EVENT value)
{
	STATUS result = ST_OK;
	if (event == PIN_EVENT) {
		if (sendPinEvent(value) != ST_OK)
			result = ST_ERR;
	} else if (event == XDP_PRESENT_EVENT) {
		// send message to network listener thread which is polling for events
		ASD_log(LogType_Debug, "Disabling remote debug.");
		uint64_t event_value = CLOSE_CLIENT_EVENT;
		int i = write(event_fd, &event_value, sizeof(event_value));
		if (i != sizeof(event_value)) {
			ASD_log(LogType_Error, "Failed to send remote debug disable event.");
			result = ST_ERR;
		}
		ASD_log(LogType_Debug, "Disable remote debug event sent.");
	} else {
		ASD_log(LogType_Error, "invalid callback event type");
		result = ST_ERR;
	}
	return result;
}

STATUS on_client_connect(ClientAddrT cl_addr)
{
	STATUS result = ST_OK;
	ASD_log(LogType_Debug, "Preparing for client connection");

	// Initialize the pin control handler
	if (target_initialize(target_control_handle, cl_addr) != ST_OK) {
		ASD_log(LogType_Error, "Failed to initialize the target_control_handle");
		result = ST_ERR;
	}

	// Initialize the jtag handler
	if (result == ST_OK && JTAG_initialize(jtag_handler) != ST_OK) {
		ASD_log(LogType_Error, "Failed to initialize the jtag_handler");
		result = ST_ERR;
	}

	if (result == ST_OK && pthread_mutex_init(&send_buffer_mutex, NULL) != 0) {
		ASD_log(LogType_Error, "Failed to init send buffer mutex");
		result = ST_ERR;
	}

	return result;
}

STATUS on_client_disconnect()
{
	STATUS result = ST_OK;
	ASD_log(LogType_Debug, "Cleaning up after client connection");

	pthread_mutex_destroy(&send_buffer_mutex);

	// Deinitialize the JTAG control handler
	if (JTAG_deinitialize(jtag_handler) != ST_OK) {
		ASD_log(LogType_Error, "Failed to deinitialize the JTAG handler");
		result = ST_ERR;
	}

	// Deinitialize the pin control handler
	if (target_control_handle && target_deinitialize(target_control_handle) != ST_OK) {
		ASD_log(LogType_Error, "Failed to deinitialize the pin control handler");
		result = ST_ERR;
	}

	return result;
}

void on_message_received(struct spi_message message, const int data_size)
{
	if (message.header.enc_bit) {
		ASD_log(LogType_Error, "enc_bit found be we don't support it!");
		send_error_message(comm_fd, &message, ASD_UNKNOWN_ERROR);
		return;
	}
	if (message.header.type == AGENT_CONTROL_TYPE) {
		memcpy(&out_msg.header,
			&message.header, sizeof(struct message_header));
		out_msg.buffer[0] = message.header.cmd_stat;

		switch (message.header.cmd_stat) {
		case NUM_IN_FLIGHT_MESSAGES_SUPPORTED_CMD:
			out_msg.buffer[1] = NUM_IN_FLIGHT_BUFFERS_TO_USE;
			out_msg.header.size_lsb = 2;
			out_msg.header.size_msb = 0;
			out_msg.header.cmd_stat = ASD_SUCCESS;
			break;
		case OBTAIN_DOWNSTREAM_VERSION_CMD:
			// The +/-1 references below are to account for the
			// write of cmd_stat to buffer position 0 above
			memcpy(&out_msg.buffer[1],
				asd_version, sizeof(asd_version));
			out_msg.header.size_lsb = sizeof(asd_version)+1;
			out_msg.header.size_msb = 0;
			out_msg.header.cmd_stat = ASD_SUCCESS;
			break;
		}
		if (send_out_msg_on_socket(&out_msg) != ST_OK) {
			ASD_log(LogType_Error, "Failed to send agent control message response.");
		}
	} else {
		process_message(comm_fd, &message);
	}
}

int main(int argc, char **argv)
{
	int pollfd_cnt = 2;
	struct sockaddr_in host_addr = {0};
	struct pollfd poll_fds[3] = {{0}};
	struct spi_message s_message = {{0}};
	int opt = 1, data_size = 0;
	SocketReadState read_state = SOCKET_READ_STATE_INITIAL;
	ssize_t read_index = 0;

	print_version();
	process_command_line(argc, argv);

	jtag_handler = SoftwareJTAGHandler();
	if (!jtag_handler) {
		ASD_log(LogType_Error, "Failed to create SoftwareJTAGHandler");
		exitAll();
		return 1;
	}

	target_control_handle = TargetHandler(&TargetHandlerCallback);
	if (!target_control_handle) {
		ASD_log(LogType_Error, "Failed to create TargetHandler");
		exitAll();
		return 1;
	}
	comm_fd = -1;

	out_msg.buffer = (unsigned char*)malloc(MAX_DATA_SIZE);
	if (!out_msg.buffer) {
		ASD_log(LogType_Error, "Failed to allocate out_msg.buffer");
		exitAll();
		return 1;
	}

	s_message.buffer = (unsigned char*)malloc(MAX_DATA_SIZE);
	if (!s_message.buffer) {
		ASD_log(LogType_Error, "Failed to allocate s_message.buffer");
		exitAll();
		return 1;
	}

	send_buffer = (char *)malloc(MAX_DATA_SIZE);
	if (!send_buffer) {
		ASD_log(LogType_Error, "Failed to create message buffer for socket write.");
		if(s_message.buffer)
			free(s_message.buffer);
		exitAll();
		return 1;
	}

	event_fd = eventfd(0, O_NONBLOCK);
	if (event_fd == -1) {
		ASD_log(LogType_Error, "Could not setup event file descriptor.");
		if (s_message.buffer)
			free(s_message.buffer);
		exitAll();
		return 1;
	}

	host_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (host_fd < 0) {
		ASD_log(LogType_Error, "Could not obtain the socket fd");
		if(s_message.buffer)
			free(s_message.buffer);
		exitAll();
		return 1;
	}
	host_addr.sin_family = AF_INET;
	host_addr.sin_addr.s_addr = INADDR_ANY;
	host_addr.sin_port = htons(port_number);

	setsockopt(host_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(int));

	if (bind(host_fd, (struct sockaddr *)&host_addr, sizeof(host_addr)) < 0) {
		ASD_log(LogType_Error, "Could not bind. Ensure that another client is "
				"not still connected.");
		if (s_message.buffer)
			free(s_message.buffer);
		exitAll();
		return 1;
	}
	listen(host_fd, 5);
	poll_fds[EVENT_FD_INDEX].fd = event_fd;
	poll_fds[EVENT_FD_INDEX].events = POLLIN;
	poll_fds[HOST_FD_INDEX].fd = host_fd;
	poll_fds[HOST_FD_INDEX].events = POLLIN;
	poll_fds[CLIENT_FD_INDEX].fd = 0;
	poll_fds[CLIENT_FD_INDEX].events = 0;

	while (1) {
		if (poll(poll_fds, pollfd_cnt, -1) == -1) {
			break;
		}

		if (poll_fds[EVENT_FD_INDEX].revents & POLLIN) {
			uint64_t value;
			int i = read(event_fd, &value, sizeof(value));
			ASD_log(LogType_Debug, "Internal event received.");
			if (i != sizeof(value)) {
				ASD_log(LogType_Error, "Error occurred receiving internal event.");
			} else {
				if (value == CLOSE_CLIENT_EVENT) {
					ASD_log(LogType_Debug, "Internal client close event received.");
					if (poll_fds[CLIENT_FD_INDEX].fd == 0) {
						ASD_log(LogType_Error, "Client already disconnected.");
					} else {
						ASD_log(LogType_Error, "Remote JTAG disabled, disconnecting client.");
						if (on_client_disconnect() != ST_OK) {
							ASD_log(LogType_Error, "Client disconnect cleanup failed.");
						}
						close(poll_fds[CLIENT_FD_INDEX].fd);
						poll_fds[CLIENT_FD_INDEX].fd = 0;
						poll_fds[CLIENT_FD_INDEX].events = 0;
						pollfd_cnt = 2;
						continue;
					}
				}
			}
		}
		if (poll_fds[HOST_FD_INDEX].revents & POLLIN) {
			struct sockaddr_in addr;
			uint len = sizeof(addr);
			ASD_log(LogType_Debug, "Client Connecting.");
			int fd = accept(host_fd, (struct sockaddr *)&addr, &len);
			if (fd == -1) {
				ASD_log(LogType_Error, "Failed to accept incoming connection.");
				continue;
			} else if (poll_fds[CLIENT_FD_INDEX].fd == 0) {
				ClientAddrT cl_addr = {0};
				int offset = sizeof(cl_addr) - sizeof(addr.sin_addr);

				memcpy(&cl_addr[offset], &addr.sin_addr, sizeof(addr.sin_addr));
				if (on_client_connect(cl_addr) != ST_OK) {
					ASD_log(LogType_Error, "Connection attempt failed.");
					if (on_client_disconnect() != ST_OK) {
						ASD_log(LogType_Error, "Client disconnect cleanup failed.");
					}
					close(fd);
					continue;
				}
				comm_fd = fd;
				poll_fds[CLIENT_FD_INDEX].fd = comm_fd;
				poll_fds[CLIENT_FD_INDEX].events = POLLIN;
				pollfd_cnt = 3;
				read_state = SOCKET_READ_STATE_INITIAL;
				ASD_log(LogType_Error, "Client connected.");
			} else {
				ASD_log(LogType_Debug, "Received 2nd connection. Refusing...");
				close(fd);
				continue;
			}
		}
		if (poll_fds[CLIENT_FD_INDEX].revents & POLLIN) {
			ASD_log(LogType_Debug, "receive from client...read_state:%u", read_state);
			switch (read_state) {
			case SOCKET_READ_STATE_INITIAL:
				memset(&s_message.header, 0, sizeof(struct message_header));
				memset(s_message.buffer, 0, MAX_DATA_SIZE);
				read_state = SOCKET_READ_STATE_HEADER;
				read_index = 0;
				// do not 'break' here, continue on and read the header
			case SOCKET_READ_STATE_HEADER: {
				ssize_t cnt = recv(comm_fd, (void*)(&(s_message.header) + read_index),
							sizeof(s_message.header) - read_index, 0);
				if (cnt < 1) {
					if (cnt == 0)
						ASD_log(LogType_Error, "Client disconnected");
					else
						ASD_log(LogType_Error, "Socket header receive failed: %d", cnt);
					if (on_client_disconnect() != ST_OK) {
						ASD_log(LogType_Error, "Client disconnect cleanup failed.");
					}
					close(poll_fds[CLIENT_FD_INDEX].fd);
					poll_fds[CLIENT_FD_INDEX].fd = 0;
					poll_fds[CLIENT_FD_INDEX].events = 0;
					pollfd_cnt = 2;
				} else if ((cnt + read_index) == sizeof(s_message.header)) {
					ASD_log(LogType_NETWORK, "cnt: %u", cnt);
					data_size = get_message_size(&s_message);
					if (prnt_net) {
						ASD_log(LogType_NETWORK,
								"Received header:  origin_id: 0x%02x\n"
								"    reserved: 0x%02x    enc_bit: 0x%02x\n"
								"    type: 0x%02x        size_lsb: 0x%02x\n"
								"    size_msb: 0x%02x    tag: 0x%02x\n"
								"    cmd_stat: 0x%02x",
								s_message.header.origin_id, s_message.header.reserved,
								s_message.header.enc_bit, s_message.header.type,
								s_message.header.size_lsb, s_message.header.size_msb,
								s_message.header.tag, s_message.header.cmd_stat);
						ASD_log(LogType_NETWORK, "Received Buffer size: %d", data_size);
						ASD_log_buffer(LogType_NETWORK, s_message.buffer, data_size, "Recv");
					}

					if (data_size == -1) {
						ASD_log(LogType_Error, "Failed to read header size.");
						send_error_message(comm_fd, &s_message, ASD_UNKNOWN_ERROR);
						if (on_client_disconnect() != ST_OK) {
							ASD_log(LogType_Error, "Client disconnect cleanup failed.");
						}
						close(poll_fds[CLIENT_FD_INDEX].fd);
						poll_fds[CLIENT_FD_INDEX].fd = 0;
						poll_fds[CLIENT_FD_INDEX].events = 0;
						pollfd_cnt = 2;
					} else if (data_size > 0) {
						read_state = SOCKET_READ_STATE_BUFFER;
						read_index = 0;
					} else {
						// we have finished reading a message and there is no buffer to read.
						// Set back to initial state for next packet and process message.
						read_state = SOCKET_READ_STATE_INITIAL;
						on_message_received(s_message, data_size);
					}
				} else {
					read_index += cnt;
					ASD_log(LogType_Debug, "Socket header read not complete (%d of %d)",
						read_index, sizeof(s_message.header));
				}
				break;
			}
			case SOCKET_READ_STATE_BUFFER: {
				ssize_t cnt = recv(comm_fd, (void*)(s_message.buffer + read_index),
							data_size - read_index, 0);
				if (cnt < 1) {
					if (cnt == 0)
						ASD_log(LogType_Error, "Client disconnected");
					else
						ASD_log(LogType_Error, "Socket buffer receive failed: %d", cnt);
					send_error_message(comm_fd, &s_message, ASD_UNKNOWN_ERROR);
					if (on_client_disconnect() != ST_OK) {
						ASD_log(LogType_Error, "Client disconnect cleanup failed.");
					}
					close(poll_fds[CLIENT_FD_INDEX].fd);
					poll_fds[CLIENT_FD_INDEX].fd = 0;
					poll_fds[CLIENT_FD_INDEX].events = 0;
					pollfd_cnt = 2;
				} else if ((cnt + read_index) == data_size) {
					// we have finished reading a packet. Set back to initial state for next packet.
					read_state = SOCKET_READ_STATE_INITIAL;
					on_message_received(s_message, data_size);
				} else {
					read_index += cnt;
					ASD_log(LogType_Debug, "Socket header read not complete (%d of %d)",
						read_index, data_size);
				}
				break;
			}
			default:
				ASD_log(LogType_Error, "Invalid socket read state: %d", read_state);
			}
		}
	}
	if (s_message.buffer)
		free(s_message.buffer);
	exitAll();
	ASD_log(LogType_Error, "ASD server closing.");
	return 0;
}

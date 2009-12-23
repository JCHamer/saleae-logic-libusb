// vim: sw=8:ts=8:noexpandtab
#include "slogic.h"
#include "usbutil.h"
#include "firmware/firmware.h"

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_N_TRANSFER_BUFFERS 4

#define DEFAULT_TRANSFER_BUFFER_SIZE 4 * 1024

// EP1 OUT
#define COMMAND_OUT_ENDPOINT 0x01
// EP1 IN
#define COMMAND_IN_ENDPOINT 0x81
// EP2 IN
#define STREAMING_DATA_IN_ENDPOINT 0x82
// EP6 OUT
#define STREAMING_DATA_OUT_ENDPOINT 0x06

//Bus 006 Device 006: ID 0925:3881 Lakeview Research
#define USB_VENDOR_ID 0x0925
#define USB_PRODUCT_ID 0x3881

struct slogic_handle {
	/* pointer to the usb handle */
	libusb_device_handle *device_handle;
	libusb_context *context;
	size_t transfer_buffer_size;
	int n_transfer_buffers;
};

/*
 * Sample Rate
 */
struct slogic_sample_rate {
	const uint8_t pause;
	const char *text;
	const unsigned int samples_per_second;
};

struct slogic_sample_rate sample_rates[] = {
	{1, "24MHz", 24000000},
	{2, "16MHz", 16000000},
	{3, "12MHz", 12000000},
	{5, "8MHz", 8000000},
	{11, "4MHz", 4000000},
	{24, "2MHz", 2000000},
	{47, "1MHz", 1000000},
	{95, "500kHz", 500000},
	{191, "250kHz", 250000},
	{239, "200kHz", 200000},
};

struct slogic_sample_rate *slogic_parse_sample_rate(const char *str)
{
	int i;

	for (i = 0;
	     i < sizeof(sample_rates) / sizeof(struct slogic_sample_rate);
	     i++) {
		struct slogic_sample_rate *sample_rate = &sample_rates[i];
		if (strcmp(sample_rate->text, str) == 0) {
			return sample_rate;
		}
	}

	return NULL;
}

/*
 * Open / Close
 */

void slogic_upload_firmware(struct slogic_handle *handle)
{
	int counter;
	unsigned int *current;
	int data_start;
	int cmd_size = 3;
	int cmd_count =
	    slogic_firm_cmds_size() / sizeof(*slogic_firm_cmds) / cmd_size;

	current = slogic_firm_cmds;
	data_start = 0;
	for (counter = 0; counter < cmd_count; counter++) {
		/* int libusb_control_transfer(libusb_device_handle *dev_handle,
		 * int8_t request_type, uint8_t request, uint16_t value, uint16_t index,
		 * unsigned char *data, uint16_t length, unsigned int timeout); 
		 */
		libusb_control_transfer(handle->device_handle, 0x40, 0XA0,
					current[INDEX_CMD_REQUEST],
					current[INDEX_CMD_VALUE],
					&slogic_firm_data[data_start],
					current[INDEX_PAYLOAD_SIZE], 4);
		data_start += current[2];
		current += cmd_size;
	}
	sleep(1);
}

/* return 1 if the firmware is uploaded 0 if not */
int slogic_is_firmware_uploaded(struct slogic_handle *handle)
{
	/* just try to perform a normal read, if this fails we assume the firmware is not uploaded */
	unsigned char out_byte = 0x05;
	int transferred;
	int ret =
	    libusb_bulk_transfer(handle->device_handle, COMMAND_OUT_ENDPOINT,
				 &out_byte, 1,
				 &transferred, 100);
	return ret == 0;	/* probably the firmware is uploaded */
}

struct slogic_handle *slogic_open()
{
	struct slogic_handle *handle = malloc(sizeof(struct slogic_handle));
	assert(handle);

	handle->transfer_buffer_size = DEFAULT_TRANSFER_BUFFER_SIZE;
	handle->n_transfer_buffers = DEFAULT_N_TRANSFER_BUFFERS;

	libusb_init(&handle->context);

	libusb_set_debug(handle->context, 3);

	handle->device_handle =
	    open_device(handle->context, USB_VENDOR_ID, USB_PRODUCT_ID);
	if (!handle->device_handle) {
		fprintf(stderr, "Failed to find the device\n");
		return NULL;
	}
	if (!slogic_is_firmware_uploaded(handle)) {
		printf("Uploading firmware restart program\n");
		slogic_upload_firmware(handle);
		libusb_close(handle->device_handle);
		libusb_exit(handle->context);
		return NULL;
	}

	return handle;
}

void slogic_close(struct slogic_handle *handle)
{
	libusb_close(handle->device_handle);
	libusb_exit(handle->context);
	free(handle);
}

void slogic_tune(struct slogic_handle *handle, size_t transfer_buffer_size,
		 unsigned int n_transfer_buffers, int libusb_debug_level)
{
	assert(handle);

	if(transfer_buffer_size) {
		handle->transfer_buffer_size = transfer_buffer_size;
	}

	if(n_transfer_buffers) {
		handle->n_transfer_buffers = n_transfer_buffers;
	}

	if(libusb_debug_level) {
		libusb_set_debug(handle->context, libusb_debug_level);
	}
}

int slogic_readbyte(struct slogic_handle *handle, unsigned char *out)
{
	int ret;
	unsigned char command = 0x05;
	int transferred;

	ret =
	    libusb_bulk_transfer(handle->device_handle, COMMAND_OUT_ENDPOINT,
				 &command, 1, &transferred, 100);
	if (ret) {
		fprintf(stderr, "libusb_bulk_transfer (out): %s\n",
			usbutil_error_to_string(ret));
		return ret;
	}

	ret =
	    libusb_bulk_transfer(handle->device_handle,
				 COMMAND_IN_ENDPOINT, out, 1,
				 &transferred, 100);
	if (ret) {
		fprintf(stderr, "libusb_bulk_transfer (in): %s\n",
			usbutil_error_to_string(ret));
		return ret;
	}
	return 0;
}

static int tcounter = 0;

struct stransfer {
	// TODO: Access to the recording probably should be synchronized
	struct slogic_recording *recording;
	struct libusb_transfer *transfer;
	int seq;
};

struct slogic_recording {
	uint8_t *samples;
	/* Number of samples we want to collect */
	size_t recording_size;
	/* Number of samples collected so far */
	int sample_count;

	struct slogic_handle *shandle;
	/* Number of USB transfers */
	int transfer_counter;

	struct stransfer *transfers;
	unsigned int n_transfer_buffers;

	int failed;
};

static struct slogic_recording *allocate_recording(struct slogic_handle *handle,
						   uint8_t * samples,
						   size_t recording_size)
{
	struct slogic_recording *recording =
	    malloc(sizeof(struct slogic_recording));
	assert(recording);

	recording->samples = samples;
	recording->recording_size = recording_size;
	recording->transfer_counter = 0;
	recording->sample_count = 0;

	recording->shandle = handle;

	recording->n_transfer_buffers = handle->n_transfer_buffers;
	recording->transfers =
	    malloc(sizeof(struct stransfer) * recording->n_transfer_buffers);

	recording->failed = 0;

	return recording;
}

static void free_recording(struct slogic_recording *recording)
{
	free(recording->transfers);
	free(recording);
}

void slogic_read_samples_callback_start_log(struct libusb_transfer
					    *transfer)
{
	printf("start log\n");
	libusb_free_transfer(transfer);
	/* free data? */
};

static int timeout_counter = 0;
void slogic_read_samples_callback(struct libusb_transfer *transfer)
{
	struct stransfer *stransfer = transfer->user_data;
	int ret;

	assert(stransfer);

	stransfer->recording->transfer_counter++;
	if (transfer->actual_length == 0) {
		if (timeout_counter++ > 10) {
			stransfer->recording->failed = 1;
		}
	}
#if 0
	if (tcounter == 200) {
		struct libusb_transfer *transfer;
		unsigned char cmd[2];
		cmd[0] = 0x01;
		//cmd[1] = DELAY_FOR_12000000;
		//cmd[1] = DELAY_FOR_8000000;
		cmd[1] = DELAY_FOR_4000000;
		//cmd[1] = DELAY_FOR_200000;
		transfer = libusb_alloc_transfer(0);	// 0 == bulk transfer
		assert(transfer);
		libusb_fill_bulk_transfer(transfer,
					  stransfer->shandle->device_handle,
					  0x01, cmd, 2,
					  slogic_read_samples_callback_start_log,
					  NULL, 10);
		ret = libusb_submit_transfer(transfer);
		if (ret) {
			fprintf(stderr, "libusb_submit_transfer: %s\n",
				usbutil_error_to_string(ret));
			return;
		}
	}
#endif

	stransfer->recording->sample_count += transfer->actual_length;
	printf("tcounter = %d, sample_count = %d, actual_length = %d, left=%.02f\n", tcounter,
	       stransfer->recording->sample_count, transfer->actual_length, 
	       ((float)stransfer->recording->sample_count)/stransfer->recording->recording_size * 100);
#if 0
	if (tcounter < 2000) {
#endif
		stransfer->seq = tcounter++;
		ret = libusb_submit_transfer(stransfer->transfer);
		if (ret) {
			fprintf(stderr, "libusb_submit_transfer: %s\n",
				usbutil_error_to_string(ret));
			return;
		}
#if 0
	} else {
		libusb_free_transfer(transfer);
		/* free data? */
	}
#endif
}

/*
 * This methods is really a proof of concept as the
 * data is not exported yet
 */
int slogic_read_samples(struct slogic_handle *handle,
			struct slogic_sample_rate *sample_rate,
			uint8_t * samples, size_t recording_size)
{
	struct libusb_transfer *transfer;
	unsigned char *buffer;
	int counter;
	int ret;

	struct slogic_recording *recording =
	    allocate_recording(handle, samples, recording_size);

	fprintf(stderr, "Starting recording for %zu samples\n", recording_size);
	fprintf(stderr, "Transfer buffers=%d, buffer size=%zu\n", recording->n_transfer_buffers, handle->transfer_buffer_size);

	// Pre-allocate transfers
	for (counter = 0; counter < recording->n_transfer_buffers; counter++) {
		buffer = malloc(handle->transfer_buffer_size);
		assert(buffer);

		transfer = libusb_alloc_transfer(0 /* we use bulk */ );
		if (transfer == NULL) {
			fprintf(stderr, "libusb_alloc_transfer failed\n");
			return -1;	// Unknown error
		}
		libusb_fill_bulk_transfer(transfer, handle->device_handle,
					  STREAMING_DATA_IN_ENDPOINT, buffer,
					  handle->transfer_buffer_size,
					  slogic_read_samples_callback,
					  &recording->transfers[counter], 4);
		recording->transfers[counter].recording = recording;
		recording->transfers[counter].transfer = transfer;
	}

	for (counter = 0; counter < recording->n_transfer_buffers; counter++) {
		recording->transfers[counter].seq = tcounter++;
		ret =
		    libusb_submit_transfer(recording->
					   transfers[counter].transfer);
		if (ret) {
			fprintf(stderr, "libusb_submit_transfer: %s\n",
				usbutil_error_to_string(ret));
			return ret;
		}
	}

	// Switch the logic to streaming read mode
	printf("pause=%d\n", sample_rate->pause);
	unsigned char command[] = { 0x01, sample_rate->pause };
	int transferred;
	ret =
	    libusb_bulk_transfer(handle->device_handle, COMMAND_OUT_ENDPOINT,
				 command, 2, &transferred, 100);
	if (ret) {
		fprintf(stderr,
			"libusb_bulk_transfer (set streaming read mode): %s\n",
			usbutil_error_to_string(ret));
		return ret;
	}
	assert(transferred == 2);

	while (recording->sample_count < recording->recording_size
	       && !recording->failed) {
		printf("Processing events...\n");
		ret = libusb_handle_events(handle->context);
		if (ret) {
			fprintf(stderr, "libusb_handle_events: %s\n",
				usbutil_error_to_string(ret));
			break;
		}
	}

	for (counter = 0; counter < recording->n_transfer_buffers; counter++) {
		libusb_cancel_transfer(recording->transfers[counter].transfer);
		libusb_free_transfer(recording->transfers[counter].transfer);
	}

	if (recording->failed) {
		printf("FAIL!\n");
	} else {
		printf("SUCCESS!\n");
	}

	printf("Total number of samples read: %i\n", recording->sample_count);
	printf("Total number of transfers: %i\n", recording->transfer_counter);

	free_recording(recording);

	return 0;
}

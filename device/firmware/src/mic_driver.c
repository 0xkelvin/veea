 #include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <lc3.h>
#include <string.h>

#include "mic_driver.h"
#include "ble_audio_service.h"

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define MIC_PCM_SAMPLES    AUDIO_PCM_SAMPLES_PER_FRAME /* 160 mono samples */
#define MIC_I2S_CHANNELS   2  /* ESP32 I2S driver requires stereo */
#define MIC_I2S_BUF_BYTES  (MIC_PCM_SAMPLES * MIC_I2S_CHANNELS * sizeof(int16_t))
#define MIC_FRAMES_PER_BUF 2  /* PDM mono: 320 samples = 2 × 160-sample LC3 frames */
#define MIC_SLAB_BLOCKS    8
#define MIC_THREAD_STACK   4096
#define MIC_FRAME_LOG_INTERVAL 100

/* ------------------------------------------------------------------ */
/* Static storage                                                       */
/* ------------------------------------------------------------------ */

K_MEM_SLAB_DEFINE_STATIC(mic_slab, MIC_I2S_BUF_BYTES, MIC_SLAB_BLOCKS, 4);

static const struct device *i2s_dev;
static volatile bool running;
static bool i2s_hw_ready;
static uint16_t frame_seq;

/* LC3 encoder */
static lc3_encoder_t lc3_enc;
static lc3_encoder_mem_16k_t lc3_enc_mem;
static uint8_t lc3_out[AUDIO_FRAME_BYTES];

/* Capture thread */
static struct k_thread mic_thread;
K_THREAD_STACK_DEFINE(mic_stack, MIC_THREAD_STACK);

/* ------------------------------------------------------------------ */
/* I2S RX configuration                                                 */
/* ------------------------------------------------------------------ */

static int i2s_configure_rx(void)
{
	struct i2s_config cfg = {
		.word_size      = 16,
		.channels       = MIC_I2S_CHANNELS,
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_FRAME_CLK_MASTER |
				  I2S_OPT_BIT_CLK_MASTER,
		.frame_clk_freq = AUDIO_SAMPLE_RATE_HZ,
		.block_size     = MIC_I2S_BUF_BYTES,
		.mem_slab       = &mic_slab,
		.timeout        = 2000,
	};

	return i2s_configure(i2s_dev, I2S_DIR_RX, &cfg);
}

/* ------------------------------------------------------------------ */
/* Lazy I2S hardware init                                               */
/* ------------------------------------------------------------------ */

static int i2s_hw_init(void)
{
	if (i2s_hw_ready) {
		return 0;
	}

	i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
	if (!device_is_ready(i2s_dev)) {
		printk("I2S0 device not ready\n");
		i2s_dev = NULL;
		return -ENODEV;
	}

	i2s_hw_ready = true;
	printk("I2S0 hardware ready\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Capture thread                                                       */
/* ------------------------------------------------------------------ */

static void mic_capture_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	printk("Mic capture thread started\n");
	int eio_count = 0;

	while (running) {
		void *pcm_buf;
		size_t pcm_size;

		int err = i2s_read(i2s_dev, &pcm_buf, &pcm_size);
		if (err) {
			if (err == -EIO) {
				eio_count++;
				if (eio_count >= 3) {
					printk("I2S EIO - recovering\n");
					i2s_trigger(i2s_dev, I2S_DIR_RX,
						    I2S_TRIGGER_PREPARE);
					i2s_configure_rx();
					i2s_trigger(i2s_dev, I2S_DIR_RX,
						    I2S_TRIGGER_START);
					eio_count = 0;
				}
				k_msleep(5);
				continue;
			}
			if (err != -EAGAIN) {
				printk("I2S read error: %d\n", err);
			}
			k_msleep(1);
			continue;
		}

		eio_count = 0;

		if (pcm_size < MIC_I2S_BUF_BYTES) {
			k_mem_slab_free(&mic_slab, pcm_buf);
			continue;
		}

		/* PDM mono mode outputs 320 sequential mono samples in the
		 * 640-byte "stereo" buffer (not interleaved L/R pairs).
		 * Encode as 2 LC3 frames of 160 samples each.
		 */
		int16_t *samples = (int16_t *)pcm_buf;
		int total_samples = pcm_size / sizeof(int16_t);

		for (int f = 0; f < MIC_FRAMES_PER_BUF; f++) {
			int16_t *frame_pcm = &samples[f * MIC_PCM_SAMPLES];

			uint16_t seq = frame_seq++;

			if ((seq % MIC_FRAME_LOG_INTERVAL) == 0U) {
				printk("PCM[%u]: %d %d %d %d %d %d %d %d\n",
				       seq, frame_pcm[0], frame_pcm[1],
				       frame_pcm[2], frame_pcm[3],
				       frame_pcm[4], frame_pcm[5],
				       frame_pcm[6], frame_pcm[7]);
			}

			int enc_err = lc3_encode(lc3_enc, LC3_PCM_FORMAT_S16,
					 frame_pcm, 1,
					 AUDIO_FRAME_BYTES, lc3_out);

			if (enc_err) {
				printk("LC3 encode error: %d\n", enc_err);
				continue;
			}

			ble_audio_service_notify_frame(lc3_out, AUDIO_FRAME_BYTES, seq);

			if ((seq % MIC_FRAME_LOG_INTERVAL) == 0U) {
				printk("Audio frame %u sent\n", seq);
			}
		}

		k_mem_slab_free(&mic_slab, pcm_buf);
	}

	printk("Mic capture thread stopped\n");
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int mic_driver_init(void)
{
	i2s_dev = NULL;
	i2s_hw_ready = false;

	lc3_enc = lc3_setup_encoder(AUDIO_FRAME_DURATION_US,
				    AUDIO_SAMPLE_RATE_HZ,
				    AUDIO_SAMPLE_RATE_HZ,
				    &lc3_enc_mem);
	if (!lc3_enc) {
		printk("LC3 encoder init failed\n");
		return -ENOMEM;
	}

	running = false;
	frame_seq = 0;
	printk("Mic driver init OK (LC3 ready, I2S0 PDM deferred)\n");
	return 0;
}

void mic_driver_start(void)
{
	if (running) {
		return;
	}

	int err = i2s_hw_init();
	if (err) {
		printk("Mic: I2S0 init failed (%d)\n", err);
		return;
	}

	err = i2s_configure_rx();
	if (err) {
		printk("I2S configure failed: %d\n", err);
		return;
	}

	err = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (err) {
		printk("I2S start failed: %d\n", err);
		return;
	}

	running = true;
	frame_seq = 0;

	k_thread_create(&mic_thread, mic_stack,
			K_THREAD_STACK_SIZEOF(mic_stack),
			mic_capture_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
	k_thread_name_set(&mic_thread, "mic_capture");

	printk("Microphone started (I2S0 PDM)\n");
}

void mic_driver_stop(void)
{
	if (!running) {
		return;
	}

	running = false;
	i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
	k_thread_join(&mic_thread, K_SECONDS(2));
	printk("Microphone stopped\n");
}

bool mic_driver_is_running(void)
{
	return running;
}

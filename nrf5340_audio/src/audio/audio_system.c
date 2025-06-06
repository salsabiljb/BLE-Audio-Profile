/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

 #include "audio_system.h"

 #include <zephyr/kernel.h>
 #include <zephyr/shell/shell.h>
 #include <data_fifo.h>
 #include <contin_array.h>
 #include <pcm_stream_channel_modifier.h>
 #include <tone.h>
 
 #include "macros_common.h"
 #include "sw_codec_select.h"
 #include "audio_datapath.h"
 #include "audio_i2s.h"
 #include "hw_codec.h"
 #include "audio_usb.h"
 #include "streamctrl.h"
 
 
 /*-------------------------*/
 
 #include <math.h>
 #include "audio_datapath.h"
 #include "streamctrl.h"
 #include "data_fifo.h" 
 #include <zephyr/drivers/gpio.h>

 /*-------------------------*/
 
 #include <zephyr/logging/log.h>
 LOG_MODULE_REGISTER(audio_system, CONFIG_AUDIO_SYSTEM_LOG_LEVEL);
 
 
 /*-------------------------*/
 
 #define NOISE_THREAD_STACK_SIZE 1024
 #define NOISE_THREAD_PRIORITY 5
 #define NOISE_SAMPLES_PER_READ 64  
 
 K_THREAD_STACK_DEFINE(noise_thread_stack, NOISE_THREAD_STACK_SIZE);
 static struct k_thread noise_thread_data;
 static bool noise_thread_running = false;



 #define RGB2_RED_NODE DT_ALIAS(led3)

 static const struct gpio_dt_spec rgb2_red = GPIO_DT_SPEC_GET(RGB2_RED_NODE, gpios);
 /*-------------------------*/
 
 
 
 
 #define FIFO_TX_BLOCK_COUNT (CONFIG_FIFO_FRAME_SPLIT_NUM * CONFIG_FIFO_TX_FRAME_COUNT)
 #define FIFO_RX_BLOCK_COUNT (CONFIG_FIFO_FRAME_SPLIT_NUM * CONFIG_FIFO_RX_FRAME_COUNT)
 
 #define DEBUG_INTERVAL_NUM     1000
 #define TEST_TONE_BASE_FREQ_HZ 1000
 
 K_THREAD_STACK_DEFINE(encoder_thread_stack, CONFIG_ENCODER_STACK_SIZE);
 
 DATA_FIFO_DEFINE(fifo_tx, FIFO_TX_BLOCK_COUNT, WB_UP(BLOCK_SIZE_BYTES));
 DATA_FIFO_DEFINE(fifo_rx, FIFO_RX_BLOCK_COUNT, WB_UP(BLOCK_SIZE_BYTES));
 
 static K_SEM_DEFINE(sem_encoder_start, 0, 1);
 
 static struct k_thread encoder_thread_data;
 static k_tid_t encoder_thread_id;
 
 static struct k_poll_signal encoder_sig;
 
 static struct k_poll_event encoder_evt =
	 K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &encoder_sig);
 
 static struct sw_codec_config sw_codec_cfg;
 /* Buffer which can hold max 1 period test tone at 1000 Hz */
 static int16_t test_tone_buf[CONFIG_AUDIO_SAMPLE_RATE_HZ / 1000];
 static size_t test_tone_size;
 
 static bool sample_rate_valid(uint32_t sample_rate_hz)
 {
	 if (sample_rate_hz == 16000 || sample_rate_hz == 24000 || sample_rate_hz == 48000) {
		 return true;
	 }
 
	 return false;
 }
 
 static void audio_gateway_configure(void)
 {
	 if (IS_ENABLED(CONFIG_SW_CODEC_LC3)) {
		 sw_codec_cfg.sw_codec = SW_CODEC_LC3;
	 } else {
		 ERR_CHK_MSG(-EINVAL, "No codec selected");
	 }
 
 #if (CONFIG_STREAM_BIDIRECTIONAL)
	 sw_codec_cfg.decoder.num_ch = 1;
	 sw_codec_cfg.decoder.channel_mode = SW_CODEC_MONO;
 #endif /* (CONFIG_STREAM_BIDIRECTIONAL) */
 
	 if (IS_ENABLED(CONFIG_MONO_TO_ALL_RECEIVERS)) {
		 sw_codec_cfg.encoder.num_ch = 1;
	 } else {
		 sw_codec_cfg.encoder.num_ch = 2;
	 }
 
	 sw_codec_cfg.encoder.channel_mode =
		 (sw_codec_cfg.encoder.num_ch == 1) ? SW_CODEC_MONO : SW_CODEC_STEREO;
 }
 
 static void audio_headset_configure(void)
 {
	 if (IS_ENABLED(CONFIG_SW_CODEC_LC3)) {
		 sw_codec_cfg.sw_codec = SW_CODEC_LC3;
	 } else {
		 ERR_CHK_MSG(-EINVAL, "No codec selected");
	 }
 
 #if (CONFIG_STREAM_BIDIRECTIONAL)
	 sw_codec_cfg.encoder.num_ch = 1;
	 sw_codec_cfg.encoder.channel_mode = SW_CODEC_MONO;
 #endif /* (CONFIG_STREAM_BIDIRECTIONAL) */
 
	 sw_codec_cfg.decoder.num_ch = 1;
	 sw_codec_cfg.decoder.channel_mode = SW_CODEC_MONO;
 
	 if (IS_ENABLED(CONFIG_SD_CARD_PLAYBACK)) {
		 /* Need an extra decoder channel to decode data from SD card */
		 sw_codec_cfg.decoder.num_ch++;
	 }
 }
 /*---------------------------------------------------------------*/
 static void noise_level_thread(void *p1, void *p2, void *p3)
 {



     gpio_pin_configure_dt(&rgb2_red, GPIO_OUTPUT_ACTIVE);



	 LOG_INF("Noise thread entered");
	 while (noise_thread_running) {
		 struct data_fifo *rx_fifo = audio_datapath_get_rx_fifo();
		 
		 if (!rx_fifo) {
			 LOG_WRN("No RX FIFO available");
			 k_sleep(K_MSEC(100));
			 continue;
		 }
 
		 const size_t bytes_per_sample = sizeof(int16_t);
		 const size_t num_bytes = NOISE_SAMPLES_PER_READ * bytes_per_sample;
 
		 void *data_ptr;
		 size_t data_size;
		 
		 int ret = data_fifo_pointer_last_filled_get(audio_datapath_get_rx_fifo(), 
												   &data_ptr, 
												   &data_size, 
												   K_NO_WAIT);
		 
		 if (ret != 0 || data_size < num_bytes) {
			 LOG_DBG("No data available (ret: %d)", ret);
			 k_sleep(K_MSEC(500));
			 continue;
		 }
 
		 int16_t *samples = (int16_t *)data_ptr;
		 uint64_t square_sum = 0;
		 size_t samples_count = MIN(data_size / bytes_per_sample, NOISE_SAMPLES_PER_READ);
 
		 for (size_t i = 0; i < samples_count; i++) {
			 square_sum += (int32_t)samples[i] * samples[i];
		 }
		 
		 float rms = sqrtf((float)square_sum / samples_count);
		 LOG_INF("Noise RMS Level: %d", (int)rms);



		 static bool led_on = false;
 
		 if (rms > 400 && !led_on) {
			 gpio_pin_set_dt(&rgb2_red, 1);
			 led_on = true;
			 LOG_INF("LED turned ON");
		 } else if (rms <= 400 && led_on) {
			 gpio_pin_set_dt(&rgb2_red, 0);
			 led_on = false;
			 LOG_INF("LED turned OFF");
		 }
		 

 
		 data_fifo_block_free(audio_datapath_get_rx_fifo(), data_ptr);
		 k_sleep(K_MSEC(1000));
	 }
 }
 /*----------------------------------------------------------------*/
 static void encoder_thread(void *arg1, void *arg2, void *arg3)
 {
	 int ret;
	 uint32_t blocks_alloced_num;
	 uint32_t blocks_locked_num;
 
	 int debug_trans_count = 0;
	 size_t encoded_data_size = 0;
 
	 void *tmp_pcm_raw_data[CONFIG_FIFO_FRAME_SPLIT_NUM];
	 char pcm_raw_data[FRAME_SIZE_BYTES];
 
	 static uint8_t *encoded_data;
	 static size_t pcm_block_size;
	 static uint32_t test_tone_finite_pos;
 
	 while (1) {
		 /* Don't start encoding until the stream needing it has started */
		 ret = k_poll(&encoder_evt, 1, K_FOREVER);
 
		 /* Get PCM data from I2S */
		 /* Since one audio frame is divided into a number of
		  * blocks, we need to fetch the pointers to all of these
		  * blocks before copying it to a continuous area of memory
		  * before sending it to the encoder
		  */

		  /*----------------------------------------------------------------------------------------------------------------------------*/
		 for (int i = 0; i < CONFIG_FIFO_FRAME_SPLIT_NUM; i++) {
			 ret = data_fifo_pointer_last_filled_get(&fifo_rx, &tmp_pcm_raw_data[i],
								 &pcm_block_size, K_FOREVER);
			 ERR_CHK(ret);
			 memcpy(pcm_raw_data + (i * BLOCK_SIZE_BYTES), tmp_pcm_raw_data[i],
					pcm_block_size);
 
			 data_fifo_block_free(&fifo_rx, tmp_pcm_raw_data[i]);
		 }
         /*----------------------------------------------------------------------------------------------------------------------------*/




		 
		 if (sw_codec_cfg.encoder.enabled) {
			 if (test_tone_size) {
				 /* Test tone takes over audio stream */
				 uint32_t num_bytes;
				 char tmp[FRAME_SIZE_BYTES / 2];
 
				 ret = contin_array_create(tmp, FRAME_SIZE_BYTES / 2, test_tone_buf,
							   test_tone_size, &test_tone_finite_pos);
				 ERR_CHK(ret);
 
				 ret = pscm_copy_pad(tmp, FRAME_SIZE_BYTES / 2,
							 CONFIG_AUDIO_BIT_DEPTH_BITS, pcm_raw_data,
							 &num_bytes);
				 ERR_CHK(ret);
			 }
 
			 ret = sw_codec_encode(pcm_raw_data, FRAME_SIZE_BYTES, &encoded_data,
						   &encoded_data_size);
 
			 ERR_CHK_MSG(ret, "Encode failed");
		 }
 
		 /* Print block usage */
		 if (debug_trans_count == DEBUG_INTERVAL_NUM) {
			 ret = data_fifo_num_used_get(&fifo_rx, &blocks_alloced_num,
							  &blocks_locked_num);
			 ERR_CHK(ret);
			 LOG_DBG(COLOR_CYAN "RX alloced: %d, locked: %d" COLOR_RESET,
				 blocks_alloced_num, blocks_locked_num);
			 debug_trans_count = 0;
		 } else {
			 debug_trans_count++;
		 }
 
		 if (sw_codec_cfg.encoder.enabled) {
			 streamctrl_send(encoded_data, encoded_data_size,
					 sw_codec_cfg.encoder.num_ch);
		 }
		 STACK_USAGE_PRINT("encoder_thread", &encoder_thread_data);
	 }
 }
 
 void audio_system_encoder_start(void)
 {
	 LOG_DBG("Encoder started");
	 k_poll_signal_raise(&encoder_sig, 0);
 }
 
 void audio_system_encoder_stop(void)
 {
	 k_poll_signal_reset(&encoder_sig);
 }
 
 int audio_system_encode_test_tone_set(uint32_t freq)
 {
	 int ret;
 
	 if (freq == 0) {
		 test_tone_size = 0;
		 return 0;
	 }
 
	 if (IS_ENABLED(CONFIG_AUDIO_TEST_TONE)) {
		 ret = tone_gen(test_tone_buf, &test_tone_size, freq, CONFIG_AUDIO_SAMPLE_RATE_HZ,
					1);
		 ERR_CHK(ret);
	 } else {
		 LOG_ERR("Test tone is not enabled");
		 return -ENXIO;
	 }
 
	 if (test_tone_size > sizeof(test_tone_buf)) {
		 return -ENOMEM;
	 }
 
	 return 0;
 }
 
 int audio_system_encode_test_tone_step(void)
 {
	 int ret;
	 static uint32_t test_tone_hz;
 
	 if (CONFIG_AUDIO_BIT_DEPTH_BITS != 16) {
		 LOG_WRN("Tone gen only supports 16 bits");
		 return -ECANCELED;
	 }
 
	 if (test_tone_hz == 0) {
		 test_tone_hz = TEST_TONE_BASE_FREQ_HZ;
	 } else if (test_tone_hz >= TEST_TONE_BASE_FREQ_HZ * 4) {
		 test_tone_hz = 0;
	 } else {
		 test_tone_hz = test_tone_hz * 2;
	 }
 
	 if (test_tone_hz != 0) {
		 LOG_INF("Test tone set at %d Hz", test_tone_hz);
	 } else {
		 LOG_INF("Test tone off");
	 }
 
	 ret = audio_system_encode_test_tone_set(test_tone_hz);
	 if (ret) {
		 LOG_ERR("Failed to generate test tone");
		 return ret;
	 }
 
	 return 0;
 }
 
 int audio_system_config_set(uint32_t encoder_sample_rate_hz, uint32_t encoder_bitrate,
				 uint32_t decoder_sample_rate_hz)
 {
	 if (sample_rate_valid(encoder_sample_rate_hz)) {
		 sw_codec_cfg.encoder.sample_rate_hz = encoder_sample_rate_hz;
	 } else if (encoder_sample_rate_hz) {
		 LOG_ERR("%d is not a valid sample rate", encoder_sample_rate_hz);
		 return -EINVAL;
	 }
 
	 if (sample_rate_valid(decoder_sample_rate_hz)) {
		 sw_codec_cfg.decoder.enabled = true;
		 sw_codec_cfg.decoder.sample_rate_hz = decoder_sample_rate_hz;
	 } else if (decoder_sample_rate_hz) {
		 LOG_ERR("%d is not a valid sample rate", decoder_sample_rate_hz);
		 return -EINVAL;
	 }
 
	 if (encoder_bitrate) {
		 sw_codec_cfg.encoder.enabled = true;
		 sw_codec_cfg.encoder.bitrate = encoder_bitrate;
	 }
 
	 return 0;
 }
 
 /* This function is only used on gateway using USB as audio source and bidirectional stream */
 int audio_system_decode(void const *const encoded_data, size_t encoded_data_size, bool bad_frame)
 {
	 int ret;
	 uint32_t blocks_alloced_num;
	 uint32_t blocks_locked_num;
	 static int debug_trans_count;
	 static void *tmp_pcm_raw_data[CONFIG_FIFO_FRAME_SPLIT_NUM];
	 static void *pcm_raw_data;
	 size_t pcm_block_size;
 
	 if (!sw_codec_cfg.initialized) {
		 /* Throw away data */
		 /* This can happen when using play/pause since there might be
		  * some packages left in the buffers
		  */
		 LOG_DBG("Trying to decode while codec is not initialized");
		 return -EPERM;
	 }
 
	 ret = data_fifo_num_used_get(&fifo_tx, &blocks_alloced_num, &blocks_locked_num);
	 if (ret) {
		 return ret;
	 }
 
	 uint8_t free_blocks_num = FIFO_TX_BLOCK_COUNT - blocks_locked_num;
 
	 /* If not enough space for a full frame, remove oldest samples to make room */
	 if (free_blocks_num < CONFIG_FIFO_FRAME_SPLIT_NUM) {
		 void *old_data;
		 size_t size;
 
		 for (int i = 0; i < (CONFIG_FIFO_FRAME_SPLIT_NUM - free_blocks_num); i++) {
			 ret = data_fifo_pointer_last_filled_get(&fifo_tx, &old_data, &size,
								 K_NO_WAIT);
			 if (ret == -ENOMSG) {
				 /* If there are no more blocks in FIFO, break */
				 break;
			 }
 
			 data_fifo_block_free(&fifo_tx, old_data);
		 }
	 }
 
	 for (int i = 0; i < CONFIG_FIFO_FRAME_SPLIT_NUM; i++) {
		 ret = data_fifo_pointer_first_vacant_get(&fifo_tx, &tmp_pcm_raw_data[i], K_FOREVER);
		 if (ret) {
			 return ret;
		 }
	 }
 
	 ret = sw_codec_decode(encoded_data, encoded_data_size, bad_frame, &pcm_raw_data,
				   &pcm_block_size);
	 if (ret) {
		 LOG_ERR("Failed to decode");
		 return ret;
	 }
 
	 /* Split decoded frame into CONFIG_FIFO_FRAME_SPLIT_NUM blocks */
	 for (int i = 0; i < CONFIG_FIFO_FRAME_SPLIT_NUM; i++) {
		 memcpy(tmp_pcm_raw_data[i], (char *)pcm_raw_data + (i * (BLOCK_SIZE_BYTES)),
				BLOCK_SIZE_BYTES);
 
		 ret = data_fifo_block_lock(&fifo_tx, &tmp_pcm_raw_data[i], BLOCK_SIZE_BYTES);
		 if (ret) {
			 LOG_ERR("Failed to lock block");
			 return ret;
		 }
	 }
	 if (debug_trans_count == DEBUG_INTERVAL_NUM) {
		 ret = data_fifo_num_used_get(&fifo_tx, &blocks_alloced_num, &blocks_locked_num);
		 if (ret) {
			 return ret;
		 }
		 LOG_DBG(COLOR_MAGENTA "TX alloced: %d, locked: %d" COLOR_RESET, blocks_alloced_num,
			 blocks_locked_num);
		 debug_trans_count = 0;
	 } else {
		 debug_trans_count++;
	 }
 
	 return 0;
 }
 
 /**@brief Initializes the FIFOs, the codec, and starts the I2S
  */
 
 void audio_system_start(void)
 {
	 int ret;
 
	 if (CONFIG_AUDIO_DEV == HEADSET) {
		 audio_headset_configure();
	 } else if (CONFIG_AUDIO_DEV == GATEWAY) {
		 audio_gateway_configure();
	 } else {
		 LOG_ERR("Invalid CONFIG_AUDIO_DEV: %d", CONFIG_AUDIO_DEV);
		 ERR_CHK(-EINVAL);
	 }
 
	 if (!fifo_tx.initialized) {
		 ret = data_fifo_init(&fifo_tx);
		 ERR_CHK_MSG(ret, "Failed to set up tx FIFO");
	 }
 
	 if (!fifo_rx.initialized) {
		 ret = data_fifo_init(&fifo_rx);
		 ERR_CHK_MSG(ret, "Failed to set up rx FIFO");
	 }
 
	 ret = sw_codec_init(sw_codec_cfg);
	 ERR_CHK_MSG(ret, "Failed to set up codec");
 
	 sw_codec_cfg.initialized = true;
 
	 if (sw_codec_cfg.encoder.enabled && encoder_thread_id == NULL) {
		 encoder_thread_id = k_thread_create(
			 &encoder_thread_data, encoder_thread_stack, CONFIG_ENCODER_STACK_SIZE,
			 (k_thread_entry_t)encoder_thread, NULL, NULL, NULL,
			 K_PRIO_PREEMPT(CONFIG_ENCODER_THREAD_PRIO), 0, K_NO_WAIT);
		 ret = k_thread_name_set(encoder_thread_id, "ENCODER");
		 ERR_CHK(ret);
	 }
 
 #if ((CONFIG_AUDIO_SOURCE_USB) && (CONFIG_AUDIO_DEV == GATEWAY))
	 ret = audio_usb_start(&fifo_tx, &fifo_rx);
	 
	 ERR_CHK(ret);
 #else
	 ret = hw_codec_default_conf_enable();
	 ERR_CHK(ret);
 
	 ret = audio_datapath_start(&fifo_rx);
	 ERR_CHK(ret);
	 
	 /*delay to make sure that the buffer isn't empty before processing its content*/
	 k_sleep(K_MSEC(100));
	 
    /*------------------------------------*/
	 
	 if (!noise_thread_running && IS_ENABLED(CONFIG_STREAM_BIDIRECTIONAL) && CONFIG_AUDIO_DEV == HEADSET) {
		 noise_thread_running = true;
		 k_thread_create(&noise_thread_data, noise_thread_stack,
						K_THREAD_STACK_SIZEOF(noise_thread_stack),
						noise_level_thread,
						NULL, NULL, NULL,
						NOISE_THREAD_PRIORITY, 0, K_NO_WAIT);
		 LOG_INF("Noise measurement thread started");
	 }
    /*------------------------------------*/

 #endif /* ((CONFIG_AUDIO_SOURCE_USB) && (CONFIG_AUDIO_DEV == GATEWAY))) */
 }
 
 void audio_system_stop(void)
 {
	 /*--------------------------------*/
	 noise_thread_running = false;
    /*------------------------------------*/
	 int ret;
 
	 if (!sw_codec_cfg.initialized) {
		 LOG_WRN("Codec already unitialized");
		 return;
	 }
 
	 LOG_DBG("Stopping codec");
 
 #if ((CONFIG_AUDIO_DEV == GATEWAY) && CONFIG_AUDIO_SOURCE_USB)
	 audio_usb_stop();
 #else
	 ret = hw_codec_soft_reset();
	 ERR_CHK(ret);
 
	 ret = audio_datapath_stop();
	 ERR_CHK(ret);
 #endif /* ((CONFIG_AUDIO_DEV == GATEWAY) && CONFIG_AUDIO_SOURCE_USB) */
 
	 ret = sw_codec_uninit(sw_codec_cfg);
	 ERR_CHK_MSG(ret, "Failed to uninit codec");
	 sw_codec_cfg.initialized = false;
 
	 data_fifo_empty(&fifo_rx);
	 data_fifo_empty(&fifo_tx);
	 
 }
 
 int audio_system_fifo_rx_block_drop(void)
 {
	 int ret;
	 void *temp;
	 size_t temp_size;
 
	 ret = data_fifo_pointer_last_filled_get(&fifo_rx, &temp, &temp_size, K_NO_WAIT);
	 if (ret) {
		 LOG_WRN("Failed to get last filled block");
		 return -ECANCELED;
	 }
 
	 data_fifo_block_free(&fifo_rx, temp);
 
	 LOG_DBG("Block dropped");
	 return 0;
 }
 
 int audio_system_decoder_num_ch_get(void)
 {
	 return sw_codec_cfg.decoder.num_ch;
 }
 
 int audio_system_init(void)
 {
	 int ret;
 
 #if ((CONFIG_AUDIO_DEV == GATEWAY) && (CONFIG_AUDIO_SOURCE_USB))
	 ret = audio_usb_init();
	 if (ret) {
		 LOG_ERR("Failed to initialize USB: %d", ret);
		 return ret;
	 }
 #else
	 ret = audio_datapath_init();
	 if (ret) {
		 LOG_ERR("Failed to initialize audio datapath: %d", ret);
		 return ret;
	 }
 
	 ret = hw_codec_init();
	 if (ret) {
		 LOG_ERR("Failed to initialize HW codec: %d", ret);
		 return ret;
	 }
 #endif
	 k_poll_signal_init(&encoder_sig);
 
	 return 0;
 }
 
 static int cmd_audio_system_start(const struct shell *shell, size_t argc, const char **argv)
 {
	 ARG_UNUSED(argc);
	 ARG_UNUSED(argv);
 
	 audio_system_start();
 
	 shell_print(shell, "Audio system started");
 
	 return 0;
 }
 
 static int cmd_audio_system_stop(const struct shell *shell, size_t argc, const char **argv)
 {
	 ARG_UNUSED(argc);
	 ARG_UNUSED(argv);
 
	 audio_system_stop();
 
	 shell_print(shell, "Audio system stopped");
 
	 return 0;
 }
 
 SHELL_STATIC_SUBCMD_SET_CREATE(audio_system_cmd,
					SHELL_COND_CMD(CONFIG_SHELL, start, NULL, "Start the audio system",
						   cmd_audio_system_start),
					SHELL_COND_CMD(CONFIG_SHELL, stop, NULL, "Stop the audio system",
						   cmd_audio_system_stop),
					SHELL_SUBCMD_SET_END);
 
 SHELL_CMD_REGISTER(audio_system, &audio_system_cmd, "Audio system commands", NULL);
 

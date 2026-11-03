#ifndef PHI_HTTP_H
#define PHI_HTTP_H
#include <curl/curl.h>

/* Run one transfer on a multi handle, polling so the keyboard and
   cancellation stay responsive without another thread. Returns the
   transfer's result, or CURLE_ABORTED_BY_CALLBACK if cancel() fired. */
static inline CURLcode phi_http_perform(CURLM *multi, CURL *easy, int (*cancel)(void)) {

	if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
		return CURLE_FAILED_INIT;
	}
	CURLMcode status = CURLM_OK;
	CURLcode result = CURLE_FAILED_INIT;
	int running = 0, aborted = 0;
	while (status == CURLM_OK) {
		if (cancel && cancel()) {
			aborted = 1;
			break;
		}
		status = curl_multi_perform(multi, &running);
		if (status != CURLM_OK || !running) {
			break;
		}
		status = curl_multi_poll(multi, NULL, 0, 50, NULL);
	}
	int remaining;
	CURLMsg *message;
	while ((message = curl_multi_info_read(multi, &remaining))) {
		if (!aborted && status == CURLM_OK && message->msg == CURLMSG_DONE &&
			message->easy_handle == easy) {
			result = message->data.result;
		}
	}
	curl_multi_remove_handle(multi, easy);

	return aborted ? CURLE_ABORTED_BY_CALLBACK : result;
}
#endif

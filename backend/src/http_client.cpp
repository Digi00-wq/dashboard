// Implementation of the libcurl wrapper (see http_client.h).

#include "http_client.h"

#include <curl/curl.h>
#include <mutex>

namespace {

std::once_flag g_curl_init;

// libcurl callback: appends the body to a string.
size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *out = static_cast<std::string *>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

} // namespace

HttpClient::HttpClient() {
  std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  curl_ = curl_easy_init();
}

HttpClient::~HttpClient() {
  if (curl_) curl_easy_cleanup(static_cast<CURL *>(curl_));
}

HttpResponse HttpClient::get(const std::string &url,
                             const std::vector<std::string> &headers) {
  return perform(url, nullptr, headers);
}

HttpResponse HttpClient::post(const std::string &url, const std::string &body,
                              const std::vector<std::string> &headers) {
  return perform(url, &body, headers);
}

HttpResponse HttpClient::perform(const std::string &url,
                                 const std::string *body,
                                 const std::vector<std::string> &headers) {
  HttpResponse res;
  auto *curl = static_cast<CURL *>(curl_);
  if (!curl) {
    res.error = "curl not initialised";
    return res;
  }

  curl_easy_reset(curl);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  struct curl_slist *list = nullptr;
  for (const auto &h : headers) list = curl_slist_append(list, h.c_str());
  if (body) {
    list = curl_slist_append(list, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body->size());
  }
  if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    res.error = curl_easy_strerror(rc);
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    const char *ct = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct)
      res.content_type = ct;
  }
  curl_slist_free_all(list);
  return res;
}

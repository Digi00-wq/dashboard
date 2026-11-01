#pragma once

#include <string>
#include <vector>

struct HttpResponse {
  long status = 0;        // 0 = transport error (see error)
  std::string body;
  std::string content_type;
  std::string error;
  bool ok() const { return status >= 200 && status < 300; }
};

// Small blocking libcurl wrapper. One instance per thread.
class HttpClient {
public:
  HttpClient();
  ~HttpClient();
  HttpClient(const HttpClient &) = delete;
  HttpClient &operator=(const HttpClient &) = delete;

  HttpResponse get(const std::string &url,
                   const std::vector<std::string> &headers = {});
  HttpResponse post(const std::string &url, const std::string &body,
                    const std::vector<std::string> &headers = {});

private:
  HttpResponse perform(const std::string &url, const std::string *body,
                       const std::vector<std::string> &headers);
  void *curl_;
};

/**
 * [학습 안내] Pi 진단 도구의 공통 HTTP SOAP 클라이언트
 * 본문 생성, 계정 파일 기반 Digest, 최대 응답 크기와 요청 Timeout, XML 파싱을 제공한다.
 * 이 도구는 터미널 진단용 동기 호출이다. PC Qt에는 QNetworkAccessManager 비동기 처리를 사용한다.
 * 실제 비밀번호는 환경의 파일 경로에서 읽고 HTTP 상태만 보고한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "soap_client.h"
#include "protocol/service.h"
#include "auth/digest.h"
#include <cstdlib>
#include "xml/xml.h"
#include <curl/curl.h>
#include <iostream>
#include <stdexcept>
namespace onvif::client {
/**
 * 클라이언트 응답 조각을 모으되 상한을 넘으면 수신을 중단한다. callback 반환값은 소비한 바이트 수다.
 */
std::size_t receive(char *data, std::size_t size, std::size_t count, void *user) {
    auto &output = *static_cast<std::string *>(user);
    const auto bytes = size * count;
    if (output.size() + bytes > 65536)
        return 0;
    output.append(data, bytes);
    return bytes;
}
/**
 * 공통 HTTP SOAP 동기 진단 요청이다. private 계정으로 Digest를 수행하고 상태/크기/Timeout을 확인한다.
 */
std::unique_ptr<onvif::Node> call(const std::string &url, const std::string &op) {
    struct CurlGlobal {
        CurlGlobal() {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
                throw std::runtime_error("Cannot initialize HTTP client");
        }
        ~CurlGlobal() {
            curl_global_cleanup();
        }
    };
    static CurlGlobal global;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        throw std::runtime_error("HTTP service URL required");
    const std::string xml = "<s:Envelope xmlns:s=\"" + std::string(onvif::SOAP) + "\" xmlns:d=\"" +
                            onvif::DEVICE + "\" xmlns:m=\"" + onvif::MEDIA + "\" xmlns:t=\"" +
                            onvif::TYPES + "\" xmlns:p=\"" + onvif::PTZ + "\" xmlns:e=\"" +
                            onvif::EVENTS + "\" xmlns:n=\"" + onvif::WSNT + "\" xmlns:pc=\"" +
                            onvif::TOPICS + "\"><s:Body>" + op + "</s:Body></s:Envelope>";
    CURL *curl = curl_easy_init();
    if (!curl)
        throw std::runtime_error("Cannot create HTTP client");
    curl_slist *headers =
        curl_slist_append(nullptr, "Content-Type: application/soap+xml; charset=utf-8");
    std::string output;
    onvif::Credentials credentials;
    if (const char *path = std::getenv("PTZ_CREDENTIALS_FILE")) {
        credentials = onvif::loadCredentials(path);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(curl, CURLOPT_USERNAME, credentials.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, credentials.password.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, xml.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(xml.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK)
        throw std::runtime_error(curl_easy_strerror(result));
    if (status != 200)
        throw std::runtime_error("SOAP HTTP status " + std::to_string(status) + ": " + output);
    return onvif::parseXml(output);
}
/**
 * namespace/local name으로 필수 자식을 찾는다. 누락된 응답을 기본값으로 조용히 성공 처리하지 않는다.
 */
const onvif::Node &required(const onvif::Node &node, const std::string &ns,
                            const std::string &local) {
    const auto *result = node.child(ns, local);
    if (!result)
        throw std::runtime_error("Missing SOAP field: " + local);
    return *result;
}
} // namespace onvif::client

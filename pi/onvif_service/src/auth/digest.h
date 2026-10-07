#pragma once
#include <map>
#include <string>
#include <chrono>
#include <cstdint>
namespace onvif {
struct Credentials { std::string username,password; };
Credentials loadCredentials(const std::string& path);
std::string hashHex(const std::string& algorithm,const std::string& input);
// HTTP Digest: 비밀번호 자체 대신 nonce와 요청 정보로 계산한 응답을 검증한다.
class DigestAuth {
public:
    explicit DigestAuth(Credentials credentials);
    std::string challenge() const;
    bool verify(const std::string& authorization,const std::string& method,const std::string& path);
private:
    Credentials credentials_;
    std::string key_;
    struct Seen { unsigned count;std::uint64_t window;std::chrono::steady_clock::time_point expiry; };
    std::map<std::string,Seen> seen_;
};
} // namespace onvif

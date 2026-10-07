/**
 * [학습 안내] HTTP Digest 인증과 재사용 요청 방지
 * realm/nonce/URI/메서드/계정으로 계산한 MD5 또는 SHA-256 응답값을 확인한다.
 * nonce는 서버 비밀키의 HMAC 서명을 갖고 120초만 유효하다. 최근 64개 nc 창은 순서 변경을 허용한다.
 * 검증은 ONVIF 입력 루프에서 실행한다. 계정 정보와 서명 키를 로그에 남기지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "auth/digest.h"
#include "xml/xml.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <ctime>
namespace onvif {
namespace {
constexpr const char *realm = "PTZ-CAMERA";
constexpr long lifetime = 120;
/**
 * 바이너리 digest/random bytes를 고정 폭 소문자 16진수로 표현한다.
 */
std::string hex(const unsigned char *data, std::size_t length) {
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < length; ++i)
        result << std::setw(2) << static_cast<unsigned>(data[i]);
    return result.str();
}
/**
 * OpenSSL의 난수 바이트를 생성한다. 실패하면 약한 대체 난수로 진행하지 않고 오류를 반환한다.
 */
std::string randomHex(unsigned size) {
    std::string bytes(size, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), size) != 1)
        throw std::runtime_error("Random generation failed");
    return hex(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
}
/**
 * 서버 비밀키로 nonce payload의 HMAC-SHA256 서명을 계산한다. 클라이언트가 timestamp/nonce를 임의 수정할 수 없게 한다.
 */
std::string mac(const std::string &key, const std::string &input) {
    unsigned char output[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (!HMAC(EVP_sha256(), key.data(), key.size(),
              reinterpret_cast<const unsigned char *>(input.data()), input.size(), output, &length))
        throw std::runtime_error("HMAC failed");
    return hex(output, length);
}
/**
 * 길이가 같은 해시 응답을 constant-time 비교한다. 단순 문자열 비교로 비밀 값의 일치 위치를 노출하지 않는다.
 */
bool equal(const std::string &a, const std::string &b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
/**
 * Digest authorization의 quoted/unquoted 필드를 파싱한다. 중복/깨진 따옴표를 거부해 모호한 인증 입력을 줄인다.
 */
std::map<std::string, std::string> fields(const std::string &header) {
    if (header.rfind("Digest ", 0) != 0)
        return {};
    std::map<std::string, std::string> result;
    std::size_t at = 7;
    while (at < header.size()) {
        while (at < header.size() && (header[at] == ' ' || header[at] == ','))
            ++at;
        const auto eq = header.find('=', at);
        if (eq == std::string::npos)
            return {};
        const auto name = trim(header.substr(at, eq - at));
        at = eq + 1;
        while (at < header.size() && header[at] == ' ')
            ++at;
        std::string value;
        if (at < header.size() && header[at] == '"') {
            ++at;
            bool closed = false;
            while (at < header.size()) {
                char c = header[at++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (at == header.size())
                        return {};
                    c = header[at++];
                }
                value += c;
            }
            if (!closed)
                return {};
            while (at < header.size() && header[at] == ' ')
                ++at;
            if (at < header.size() && header[at] != ',')
                return {};
        } else {
            const auto end = header.find(',', at);
            value = trim(header.substr(at, end == std::string::npos ? end : end - at));
            at = end == std::string::npos ? header.size() : end;
        }
        if (name.empty() || !result.emplace(name, value).second)
            return {};
    }
    return result;
}
} // namespace
/**
 * private 파일의 계정 두 필드를 읽고 형식을 검증한다. 내용을 출력하지 않으며 읽기 실패는 운영 시작 실패로 전달한다.
 */
Credentials loadCredentials(const std::string &path) {
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Cannot read credentials file");
    Credentials result;
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("username=", 0) == 0)
            result.username = line.substr(9);
        else if (line.rfind("password=", 0) == 0)
            result.password = line.substr(9);
        else if (!line.empty())
            throw std::runtime_error("Invalid credentials file format");
    }
    if (result.username.empty() || result.password.empty() || result.username.size() > 64 ||
        result.password.size() > 256)
        throw std::runtime_error("Empty or oversized credentials");
    for (unsigned char c : result.username)
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.')
            throw std::runtime_error("Invalid username");
    return result;
}
/**
 * 요청한 MD5/SHA-256 알고리즘으로 입력의 digest를 계산한다. 다른 알고리즘 이름은 명시적으로 거부한다.
 */
std::string hashHex(const std::string &algorithm, const std::string &input) {
    const EVP_MD *md = algorithm == "SHA-256" ? EVP_sha256()
                       : algorithm == "MD5"   ? EVP_md5()
                                              : nullptr;
    if (!md)
        throw std::runtime_error("Unsupported digest algorithm");
    unsigned char output[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (EVP_Digest(input.data(), input.size(), output, &length, md, nullptr) != 1)
        throw std::runtime_error("Digest failed");
    return hex(output, length);
}
/**
 * 운영 계정을 보관하고 이번 프로세스의 nonce 서명 키를 난수로 만든다. 재시작 전 nonce는 이후 유효하지 않다.
 */
DigestAuth::DigestAuth(Credentials credentials)
    : credentials_(std::move(credentials)), key_(randomHex(32)) {}
/**
 * 현재 timestamp/난수/서명 nonce와 지원 알고리즘/qop를 WWW-Authenticate로 구성한다. 비밀번호를 헤더에 넣지 않는다.
 */
std::string DigestAuth::challenge() const {
    const auto payload = std::to_string(std::time(nullptr)) + "-" + randomHex(16);
    const auto nonce = payload + "-" + mac(key_, payload);
    std::string result;
    for (const auto *algorithm : {"SHA-256", "MD5"})
        result += "WWW-Authenticate: Digest realm=\"" + std::string(realm) + "\", nonce=\"" +
                  nonce + "\", algorithm=" + algorithm + ", qop=\"auth\", charset=UTF-8\r\n";
    return result;
}
/**
 * 계정/URI/qop/서명/만료/응답 해시를 확인한 뒤 nc replay 창을 갱신한다. 실패한 요청은 성공 상태로 기록하지 않는다.
 */
bool DigestAuth::verify(const std::string &authorization, const std::string &method,
                        const std::string &path) {
    auto f = fields(authorization);
    if (!f.count("algorithm"))
        f["algorithm"] = "MD5";
    if (f["username"] != credentials_.username || f["realm"] != realm || f["uri"] != path ||
        f["qop"] != "auth" || f["cnonce"].empty() || f["cnonce"].size() > 128 ||
        f["nc"].size() != 8 || (f["algorithm"] != "SHA-256" && f["algorithm"] != "MD5"))
        return false;
    const auto nonce = f["nonce"];
    const auto first = nonce.find('-');
    const auto last = nonce.rfind('-');
    if (first == std::string::npos || last == first || nonce.size() > 160)
        return false;
    long stamp = 0;
    const auto p = std::from_chars(nonce.data(), nonce.data() + first, stamp);
    const auto now = std::time(nullptr);
    if (p.ec != std::errc{} || p.ptr != nonce.data() + first || stamp > now ||
        now - stamp > lifetime || !equal(mac(key_, nonce.substr(0, last)), nonce.substr(last + 1)))
        return false;
    unsigned count = 0;
    const auto parsed = std::from_chars(f["nc"].data(), f["nc"].data() + 8, count, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != f["nc"].data() + 8 || count == 0)
        return false;
    const auto algorithm = f["algorithm"];
    const auto ha1 =
        hashHex(algorithm, credentials_.username + ":" + realm + ":" + credentials_.password);
    const auto ha2 = hashHex(algorithm, method + ":" + path);
    const auto expected =
        hashHex(algorithm, ha1 + ":" + nonce + ":" + f["nc"] + ":" + f["cnonce"] + ":auth:" + ha2);
    if (!equal(expected, f["response"]))
        return false;
    const auto steadyNow = std::chrono::steady_clock::now();
    for (auto it = seen_.begin(); it != seen_.end();)
        if (it->second.expiry < steadyNow)
            it = seen_.erase(it);
        else
            ++it;
    const auto replayKey = nonce + ":" + f["cnonce"] + ":" + algorithm;
    const auto seen = seen_.find(replayKey);
    if (seen == seen_.end()) {
        if (seen_.size() >= 4096)
            return false;
        seen_[replayKey] = {count, 1, steadyNow + std::chrono::seconds(lifetime)};
    } else {
        // 병렬 HTTP 요청의 도착 순서가 바뀌어도 허용하되 같은 nc는 다시 받지 않는다.
        auto &state = seen->second;
        if (count > state.count) {
            const auto distance = count - state.count;
            state.window = (distance >= 64 ? 0 : state.window << distance) | 1;
            state.count = count;
        } else {
            const auto distance = state.count - count;
            if (distance >= 64 || (state.window & (std::uint64_t{1} << distance)))
                return false;
            state.window |= std::uint64_t{1} << distance;
        }
        state.expiry = steadyNow + std::chrono::seconds(lifetime);
    }
    return true;
}
} // namespace onvif

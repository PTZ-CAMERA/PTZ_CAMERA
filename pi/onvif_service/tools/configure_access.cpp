/**
 * [학습 안내] 저장소 밖 접속 계정과 MediaMTX 인증 설정 생성
 * 비밀번호를 stdin으로 받고 임시 0600 파일을 fsync/rename하여 반영한다.
 * root 권한으로 실행한다. 소스 YAML에는 실제 비밀번호를 넣지 않고 runtime 설정만 private 파일로 만든다.
 * 실행 파일/계정 정보 변경 후 해당 서비스를 재시작해야 프로세스에 반영된다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
// 비밀번호는 argv/로그에 넣지 않는다. stdin으로 받아 root 전용 파일에 기록한다.
#include "auth/digest.h"
#include "config/app_config.h"
#include <filesystem>
#include <fstream>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
/**
 * 0600 임시 파일에 전체 데이터를 쓰고 sync/rename한다. 중간 실패 때 기존 설정을 반쯤 덮어쓰지 않는다.
 */
void atomicWrite(const std::string &path, const std::string &data) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
    const std::string temporary = path + ".tmp." + std::to_string(getpid());
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        throw std::runtime_error("Cannot create private temporary file");
    std::size_t at = 0;
    while (at < data.size()) {
        const auto count = write(fd, data.data() + at, data.size() - at);
        if (count <= 0) {
            close(fd);
            unlink(temporary.c_str());
            throw std::runtime_error("Cannot write private configuration");
        }
        at += count;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(temporary.c_str());
        throw std::runtime_error("Cannot sync configuration");
    }
    close(fd);
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        throw std::runtime_error("Cannot replace configuration");
    }
}
/**
 * YAML single-quoted scalar로 계정 값을 인코딩한다. 따옴표 때문에 설정 구조가 바뀌지 않게 한다.
 */
std::string quote(const std::string &input) {
    std::string result = "'";
    for (char c : input) {
        if (c == '\'')
            result += "''";
        else
            result += c;
    }
    return result + "'";
}
} // namespace
/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    try {
        if (geteuid() != 0)
            throw std::runtime_error("Run credential configuration with sudo");
        if (argc == 4 && (std::string(argv[1]) == "--init" || std::string(argv[1]) == "--rotate")) {
            const std::string path = argv[2], username = argv[3];
            if (std::string(argv[1]) == "--init" && std::filesystem::exists(path))
                throw std::runtime_error("Credentials already exist; use --rotate deliberately");
            if (username.empty() || username.size() > 64)
                throw std::runtime_error("Invalid username length");
            for (unsigned char c : username)
                if (!std::isalnum(c) && c != '_' && c != '-' && c != '.')
                    throw std::runtime_error("Invalid username");
            std::string password;
            std::getline(std::cin, password);
            if (password.empty() || password.size() > 256 ||
                password.find('\r') != std::string::npos ||
                password.find('\0') != std::string::npos)
                throw std::runtime_error("Invalid password input");
            atomicWrite(path, "username=" + username + "\npassword=" + password + "\n");
            onvif::loadCredentials(path);
            std::cout << "Credentials written to private file (mode 0600)\n";
        } else if (argc == 3 && std::string(argv[1]) == "--validate-config") {
            edge::loadAppConfig(argv[2]);
            std::cout << "Camera configuration valid; no hardware opened\n";
        } else if (argc == 5 && std::string(argv[1]) == "--render-media") {
            edge::loadDefaultConfig();
            const auto credentials = onvif::loadCredentials(argv[2]);
            std::ifstream file(argv[3]);
            if (!file)
                throw std::runtime_error("Cannot read MediaMTX template");
            std::string base((std::istreambuf_iterator<char>(file)), {});
            // 저장소 기본값은 loopback만 허용한다. 실제 계정 블록으로 교체한다.
            const std::string beginMarker = "# BEGIN LOCAL_AUTH_DEFAULT";
            const std::string endMarker = "# END LOCAL_AUTH_DEFAULT";
            const auto begin = base.find(beginMarker);
            if (begin != std::string::npos) {
                const auto end = base.find(endMarker, begin);
                if (end == std::string::npos)
                    throw std::runtime_error("Incomplete authentication template block");
                base.erase(begin, end + endMarker.size() - begin);
            }
            if (base.find("authInternalUsers:") != std::string::npos ||
                base.find("rtspAuthMethods:") != std::string::npos)
                throw std::runtime_error(
                    "Use base template without generated authentication block");
            const auto& settings=edge::appConfig();
            const auto pathKey=base.find("  cam:\n");
            if (pathKey==std::string::npos) throw std::runtime_error("Expected cam path in base template");
            base.replace(pathKey,7,"  "+settings.rtspPath+":\n");
            const auto rtspKey=base.find("rtspAddress:");
            if (rtspKey==std::string::npos)
                base="rtspAddress: :"+std::to_string(settings.rtspPort)+"\n"+base;
            else {
                const auto end=base.find('\n',rtspKey);
                base.replace(rtspKey,end==std::string::npos?std::string::npos:end-rtspKey,
                    "rtspAddress: :"+std::to_string(settings.rtspPort));
            }
            base += "\n# Generated authentication block. Private file: do not copy into Git.\n"
                    "authMethod: internal\nrtspAuthMethods: [digest]\nauthInternalUsers:\n"
                    "  - user: any\n    pass: ''\n    ips: ['127.0.0.1', '::1']\n    permissions:\n"
                    "      - action: publish\n        path: "+settings.rtspPath+
                    "\n      - action: read\n        path: "+settings.rtspPath+"\n      - action: metrics\n"
                    "  - user: "+quote(credentials.username)+"\n    pass: "+quote(credentials.password)+
                    "\n    ips: []\n    permissions:\n      - action: read\n        path: "+settings.rtspPath+"\n";
            atomicWrite(argv[4], base);
            std::cout << "MediaMTX authenticated configuration written (mode 0600)\n";
        } else
            throw std::runtime_error("Usage: --init FILE USER | --rotate FILE USER | "
                                     "--render-media CREDENTIALS BASE_YAML OUTPUT");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

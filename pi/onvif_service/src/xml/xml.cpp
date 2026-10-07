/**
 * [학습 안내] namespace-aware XML 파싱과 XML 출력 escape
 * Expat 콜백으로 Node 트리를 구성한다. 접두어 이름 대신 namespace URI/local name으로 찾는다.
 * unique_ptr가 자식 트리 소유권을 관리한다. 파서는 요청별로 만들고 해제한다.
 * 본문 크기/깊이/노드 수를 제한하고 DTD를 거부한다. 일반 문자열 검색으로 SOAP 종류를 판별하지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "xml/xml.h"
#include <expat.h>
#include <stdexcept>
namespace onvif {
/**
 * 직접 자식 중 namespace URI와 local name이 모두 맞는 첫 노드를 찾는다. 접두어 이름은 비교하지 않는다.
 */
const Node *Node::child(const std::string &space, const std::string &local) const {
    for (const auto &item : children)
        if (item->ns == space && item->name == local)
            return item.get();
    return nullptr;
}
/**
 * XML/헤더 텍스트 앞뒤 공백만 제거한다. 숫자/토큰의 나머지 유효성은 상위 계층에서 검사한다.
 */
std::string trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
/**
 * XML에 넣는 일반 문자열의 &, <, >, 따옴표를 entity로 바꾼다. 이미 구성된 XML 전체를 escape하는 함수는 아니다.
 */
std::string escape(const std::string &input) {
    std::string result;
    for (char c : input)
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\'':
            result += "&apos;";
            break;
        default:
            result += c;
        }
    return result;
}
namespace {
struct Parser {
    XML_Parser parser;
    std::unique_ptr<Node> root;
    std::vector<Node *> stack;
    std::map<std::string, std::string> pending;
    unsigned count = 0;
};
/**
 * Expat이 namespace separator로 확장한 이름을 URI/local 두 부분으로 나눈다.
 */
std::pair<std::string, std::string> split(const char *value) {
    const std::string name(value);
    const auto at = name.find('\x1f');
    if (at == std::string::npos)
        return {{}, name};
    return {name.substr(0, at), name.substr(at + 1)};
}
/**
 * 다음 element에 적용될 prefix→URI 선언을 보관한다. QName 텍스트의 alias도 올바르게 해석하기 위한 자료다.
 */
void namespaceStart(void *user, const char *prefix, const char *uri) {
    auto &p = *static_cast<Parser *>(user);
    p.pending[prefix ? prefix : ""] = uri ? uri : "";
}
/**
 * 깊이/노드 수를 검사한 뒤 Node를 만들고 부모에 소유권을 붙인다. 자식은 부모 namespace 선언을 상속한다.
 */
void start(void *user, const char *name, const char **attrs) {
    auto &p = *static_cast<Parser *>(user);
    if (p.stack.size() >= 32 || ++p.count > 512) {
        XML_StopParser(p.parser, XML_FALSE);
        return;
    }
    auto node = std::make_unique<Node>();
    auto names = split(name);
    node->ns = names.first;
    node->name = names.second;
    // Probe Types 텍스트 속 QName도 해석할 수 있도록 해당 위치의 namespace 선언을 저장한다.
    if (!p.stack.empty())
        node->prefixes = p.stack.back()->prefixes;
    for (const auto &[key, value] : p.pending)
        node->prefixes[key] = value;
    p.pending.clear();
    for (int i = 0; attrs[i]; i += 2)
        node->attributes[attrs[i]] = attrs[i + 1];
    Node *raw = node.get();
    if (p.stack.empty())
        p.root = std::move(node);
    else
        p.stack.back()->children.push_back(std::move(node));
    p.stack.push_back(raw);
}
/**
 * 완성된 element를 파서 stack에서 꺼낸다. Node 메모리는 unique_ptr 트리에 남아 요청 해석에 사용된다.
 */
void end(void *user, const char *) {
    auto &p = *static_cast<Parser *>(user);
    if (!p.stack.empty())
        p.stack.pop_back();
}
/**
 * Expat이 나눠 전달하는 character data를 현재 Node에 누적한다.
 * 이 단계에서 trim하면 조각 사이 공백이 사라질 수 있으므로 완성 후 상위 필드 처리에서 정리한다.
 */
void text(void *user, const char *data, int length) {
    auto &p = *static_cast<Parser *>(user);
    if (!p.stack.empty())
        p.stack.back()->text.append(data, length);
}
/**
 * DTD 선언을 발견하면 파싱을 중단한다. ONVIF 요청에 불필요한 entity 확장을 허용하지 않는다.
 */
void doctype(void *user, const char *, const char *, const char *, int) {
    // ONVIF 메시지에는 DTD가 필요 없다. 외부/내부 엔티티 선언도 거부한다.
    XML_StopParser(static_cast<Parser *>(user)->parser, XML_FALSE);
}
} // namespace
/**
 * 크기 제한과 Expat 콜백을 설정하고 XML 전체를 파싱한다. 성공은 소유권 있는 root, 실패는 예외로 반환한다.
 */
std::unique_ptr<Node> parseXml(const std::string &input) {
    if (input.empty() || input.size() > 65536)
        throw std::runtime_error("XML body exceeds limit");
    // Expat이 prefix:local을 namespace URI와 local name으로 확장해 전달하게 한다.
    Parser context{XML_ParserCreateNS(nullptr, '\x1f'), {}, {}, {}, 0};
    if (!context.parser)
        throw std::runtime_error("Cannot create XML parser");
    XML_SetUserData(context.parser, &context);
    XML_SetElementHandler(context.parser, start, end);
    XML_SetCharacterDataHandler(context.parser, text);
    XML_SetNamespaceDeclHandler(context.parser, namespaceStart, nullptr);
    XML_SetStartDoctypeDeclHandler(context.parser, doctype);
    const bool ok = XML_Parse(context.parser, input.data(), static_cast<int>(input.size()),
                              XML_TRUE) == XML_STATUS_OK;
    XML_ParserFree(context.parser);
    if (!ok || !context.root)
        throw std::runtime_error("Invalid XML");
    return std::move(context.root);
}
} // namespace onvif

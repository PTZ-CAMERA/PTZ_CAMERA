#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace onvif {
struct Node {
    std::string ns, name, text;
    std::map<std::string, std::string> attributes, prefixes;
    std::vector<std::unique_ptr<Node>> children;
    const Node* child(const std::string& space, const std::string& local) const;
};
std::unique_ptr<Node> parseXml(const std::string& input);
std::string escape(const std::string& input);
std::string trim(const std::string& input);
} // namespace onvif

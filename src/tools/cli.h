#pragma once

#include <string>
#include <vector>

#include "audio/wasapi_utils.h"

namespace anc {

// Minimal command-line parser: tokens of the form "--name value".
class Args {
public:
    Args(int argc, wchar_t** argv) {
        for (int i = 1; i < argc; ++i) tokens_.emplace_back(argv[i]);
    }

    const std::vector<std::wstring>& tokens() const { return tokens_; }

    bool has(const std::wstring& name) const {
        for (const auto& t : tokens_) {
            if (t == name) return true;
        }
        return false;
    }

    std::wstring get(const std::wstring& name, const std::wstring& defaultValue = L"") const {
        for (size_t i = 0; i + 1 < tokens_.size(); ++i) {
            if (tokens_[i] == name) {
                if (!tokens_[i + 1].empty() && tokens_[i + 1][0] == L'-') return defaultValue;
                return tokens_[i + 1];
            }
        }
        return defaultValue;
    }

    double getDouble(const std::wstring& name, double defaultValue) const {
        const std::wstring v = get(name);
        if (v.empty()) return defaultValue;
        try {
            return std::stod(v);
        } catch (...) {
            return defaultValue;
        }
    }

    int getInt(const std::wstring& name, int defaultValue) const {
        const std::wstring v = get(name);
        if (v.empty()) return defaultValue;
        try {
            return std::stoi(v);
        } catch (...) {
            return defaultValue;
        }
    }

    float getFloat(const std::wstring& name, float defaultValue) const {
        return static_cast<float>(getDouble(name, static_cast<double>(defaultValue)));
    }

private:
    std::vector<std::wstring> tokens_;
};

}  // namespace anc

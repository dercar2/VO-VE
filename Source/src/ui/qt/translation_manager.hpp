#pragma once

#include "ui_text.hpp"

#include <QString>

#include <memory>

class QTranslator;

namespace vove::ui {

inline constexpr auto translationSourceDigest =
    "ef12c40b9fed7d0dba009af3a0809b7ef411ef4e2e7000c558f85708c85208cf";

class TranslationManager final {
  public:
    explicit TranslationManager(const QString &translation_directory = {});
    ~TranslationManager();

    TranslationManager(const TranslationManager &) = delete;
    TranslationManager &operator=(const TranslationManager &) = delete;

    [[nodiscard]] bool activate(AppLanguage language);
    [[nodiscard]] bool available(AppLanguage language) const;
    [[nodiscard]] AppLanguage active_language() const noexcept;
    [[nodiscard]] QString translation_directory() const;

  private:
    [[nodiscard]] QString package_path(AppLanguage language) const;

    QString translationDirectory_;
    std::unique_ptr<QTranslator> translator_;
    AppLanguage activeLanguage_{AppLanguage::english};
};

} // namespace vove::ui

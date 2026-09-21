#include "translation_manager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTranslator>

namespace vove::ui {
namespace {

[[maybe_unused]] constexpr auto languagePackSentinel =
    QT_TRANSLATE_NOOP("TranslationMetadata", "VOVE_TRANSLATION_PACK_COMPLETE");

QString default_translation_directory() {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("translations"));
}

bool catalog_is_complete(const QTranslator &translator, const AppLanguage language) {
    return translator.translate("TranslationMetadata", "VOVE_TRANSLATION_PACK_COMPLETE") ==
           language_code(language) + QLatin1Char('|') +
               QString::fromLatin1(translationSourceDigest);
}

} // namespace

TranslationManager::TranslationManager(const QString &translation_directory)
    : translationDirectory_(translation_directory.isEmpty()
                                ? default_translation_directory()
                                : QDir::cleanPath(translation_directory)) {}

TranslationManager::~TranslationManager() {
    if (translator_ != nullptr) {
        QCoreApplication::removeTranslator(translator_.get());
    }
}

bool TranslationManager::activate(const AppLanguage language) {
    if (language == activeLanguage_) {
        return true;
    }

    std::unique_ptr<QTranslator> replacement;
    if (language != AppLanguage::english) {
        replacement = std::make_unique<QTranslator>();
        if (!replacement->load(package_path(language)) ||
            !catalog_is_complete(*replacement, language)) {
            return false;
        }
    }

    if (translator_ != nullptr) {
        QCoreApplication::removeTranslator(translator_.get());
    }
    if (replacement != nullptr && !QCoreApplication::installTranslator(replacement.get())) {
        if (translator_ != nullptr) {
            static_cast<void>(QCoreApplication::installTranslator(translator_.get()));
        }
        return false;
    }

    translator_ = std::move(replacement);
    activeLanguage_ = language;
    return true;
}

bool TranslationManager::available(const AppLanguage language) const {
    if (language == AppLanguage::english) {
        return true;
    }
    const auto path = package_path(language);
    if (!QFileInfo::exists(path)) {
        return false;
    }
    QTranslator probe;
    return probe.load(path) && catalog_is_complete(probe, language);
}

AppLanguage TranslationManager::active_language() const noexcept {
    return activeLanguage_;
}

QString TranslationManager::translation_directory() const {
    return translationDirectory_;
}

QString TranslationManager::package_path(const AppLanguage language) const {
    auto code = language_code(language);
    code.replace(QLatin1Char('-'), QLatin1Char('_'));
    return QDir(translationDirectory_).filePath(QStringLiteral("vove_%1.qm").arg(code));
}

} // namespace vove::ui

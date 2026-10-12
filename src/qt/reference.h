// The flag reference (F1): the same table --help prints, wrapped to the window's real width,
// with the line naming where each flag lives in the window turned on.
//
// A separate window rather than a panel: fifty-odd flags with their prose do not fit a dock,
// and a reference read four words at a time is a reference nobody reaches the end of. The
// filter matches the flag, the text and the control column, so "tests" finds everything the
// tests dock runs and "subscribe" finds the two routes that do.

#pragma once

#include <QDialog>
#include <QFontMetrics>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <string>
#include <vector>

#include "../help.h"
#include "fonts.h"

namespace simviz {
namespace qtui {

class ReferenceWindow : public QDialog {
 public:
  explicit ReferenceWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle("oni-sim-visualizer -- flag reference");
    setModal(false);
    auto* lay = new QVBoxLayout(this);
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText("filter: a flag, a word, or a dock name");
    filter_->setClearButtonEnabled(true);
    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    text_->setFont(QFont(qtfonts::Mono(), font().pixelSize()));
    lay->addWidget(filter_);
    lay->addWidget(text_);
    QObject::connect(filter_, &QLineEdit::textChanged, [this](const QString&) { Rebuild(); });
    resize(900, 700);
  }

 protected:
  void resizeEvent(QResizeEvent* e) override {
    QDialog::resizeEvent(e);
    Rebuild();
  }
  void showEvent(QShowEvent* e) override {
    QDialog::showEvent(e);
    filter_->setFocus();
    Rebuild();
  }

 private:
  // Rebuilt only when the width in characters or the filter changed; a new filter is a new
  // document, so it starts at the top rather than wherever the last one was scrolled to.
  void Rebuild() {
    const QFontMetrics fm(text_->font());
    const int adv = std::max(1, fm.horizontalAdvance(QChar('0')));
    const int cols = std::max(40, (text_->viewport()->width() - 8) / adv);
    const std::string filter = filter_->text().toStdString();
    if (cols == cols_ && filter == filter_built_) return;
    cols_ = cols;
    filter_built_ = filter;
    const std::vector<std::string> lines = help::Reference(cols, filter, true);
    QString all;
    for (const std::string& l : lines) {
      all += QString::fromStdString(l);
      all += '\n';
    }
    text_->setPlainText(all);
  }

  QLineEdit* filter_;
  QPlainTextEdit* text_;
  int cols_ = 0;
  std::string filter_built_ = "\x01";
};

}  // namespace qtui
}  // namespace simviz

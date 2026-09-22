#include "core/main_thread_dispatch.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <utility>

namespace baniphelper::core {

void runOnMainThread(std::function<void()> action) {
  if (!action) {
    return;
  }

  QCoreApplication* application = QCoreApplication::instance();
  if (application == nullptr) {
    action();
    return;
  }

  QMetaObject::invokeMethod(application, std::move(action), Qt::QueuedConnection);
}

}  // namespace baniphelper::core

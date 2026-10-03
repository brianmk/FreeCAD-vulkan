/**************************************************************************\
 * Copyright (c) Kongsberg Oil & Gas Technologies AS
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
\**************************************************************************/

/*!  \class SIM::Coin3D::Quarter::EventFilter EventFilter.h Quarter/eventhandlers/EventFilter.h

*/

#include <QElapsedTimer>
#include <QEvent>
#include <QMouseEvent>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

#include <Inventor/SbTime.h>
#include <Inventor/events/SoLocation2Event.h>

#include "devices/Keyboard.h"
#include "devices/Mouse.h"
#include "devices/SpaceNavigatorDevice.h"
#include "eventhandlers/EventFilter.h"


namespace SIM { namespace Coin3D { namespace Quarter {

namespace {

QPointF getLocalPosition(const QMouseEvent* event)
{
  return event->position();
}

// A hover pick is synchronous (the whole scene traversal runs before the next
// Qt event is read), so if the pointer moves faster than picks complete the
// queued QMouseEvents are already obsolete when they are dispatched.  Dropping
// a pure-hover move whose platform timestamp is older than this lets the queue
// drain to the latest position instead of picking thousands of stale points
// the user never sees.  A dropped move's position is still picked once the
// pointer comes to rest (see EventFilterP::flushPendingHover()).  Overridable
// via FC_HOVER_STALE_MS; 0 disables it.
int staleHoverMoveMs()
{
  static const int ms = []() {
    const char* env = std::getenv("FC_HOVER_STALE_MS");
    return env ? std::atoi(env) : 60;
  }();
  return ms;
}

bool hoverTimingEnabled()
{
  static const bool enabled = std::getenv("FC_HOVER_TIMING") != nullptr;
  return enabled;
}

}  // namespace

class EventFilterP {
public:
  QList<InputDevice *> devices;
  InputDeviceHost * host;
  QPoint globalmousepos;
  SbVec2s windowsize;

  // Trailing hover pick: a stale move is dropped but its position is kept and
  // re-picked once the event loop goes idle, so the position where the pointer
  // comes to rest is always resolved even though its own event was stale.
  QTimer * hoverFlush = nullptr;
  bool hoverPending = false;
  SbVec2s hoverPendingPos;

  void trackWindowSize(QResizeEvent * event)
  {
    this->windowsize = SbVec2s(event->size().width(),
                               event->size().height());

    Q_FOREACH(InputDevice * device, this->devices) {
      device->setWindowSize(this->windowsize);
    }
  }

  void trackPointerPosition(QMouseEvent * event)
  {
    assert(this->windowsize[1] != -1);
    this->globalmousepos = event->globalPosition().toPoint();

    const SbVec2s windowLogical = host->vulkanDevicePixels()
        ? host->inputWindowSize()
        : this->windowsize;
    SbVec2s mousepos = InputDevice::toDevicePixelPosition(
        getLocalPosition(event),
        windowLogical,
        host->devicePixelRatio());
    Q_FOREACH(InputDevice * device, this->devices) {
      device->setMousePosition(mousepos);
    }
  }

  // Remember a dropped (stale) hover position and schedule a trailing pick that
  // runs when the event loop is otherwise idle -- i.e. when the pointer has
  // come to rest.  This gives the resting position priority over the stream of
  // stale moves that are being discarded.
  void deferPendingHover(QObject * owner, const SbVec2s & pos)
  {
    this->hoverPending = true;
    this->hoverPendingPos = pos;
    if (!this->hoverFlush) {
      this->hoverFlush = new QTimer(owner);
      this->hoverFlush->setSingleShot(true);
      this->hoverFlush->setInterval(0);
      QObject::connect(this->hoverFlush, &QTimer::timeout, owner,
                       [this]() { this->flushPendingHover(); });
    }
    this->hoverFlush->start();
  }

  void flushPendingHover()
  {
    if (!this->hoverPending || !this->host) {
      return;
    }
    this->hoverPending = false;
    SoLocation2Event hovered;
    hovered.setPosition(this->hoverPendingPos);
    hovered.setTime(SbTime::getTimeOfDay());
    if (hoverTimingEnabled()) {
      std::fprintf(stderr, "[HOVERT] flush resting pick (%d,%d)\n",
                   int(this->hoverPendingPos[0]), int(this->hoverPendingPos[1]));
    }
    this->host->processSoEvent(&hovered);
  }

  void clearPendingHover()
  {
    this->hoverPending = false;
    if (this->hoverFlush) {
      this->hoverFlush->stop();
    }
  }
};

#define PRIVATE(obj) obj->pimpl

}}} // namespace

using namespace SIM::Coin3D::Quarter;

EventFilter::EventFilter(QObject * parent)
  : QObject(parent)
{
  PRIVATE(this) = new EventFilterP;

  InputDeviceHost* host = dynamic_cast<InputDeviceHost *>(parent);
  PRIVATE(this)->host = host;
  assert(PRIVATE(this)->host);

  PRIVATE(this)->windowsize = SbVec2s(PRIVATE(this)->host->inputSize().width(),
                                      PRIVATE(this)->host->inputSize().height());

  PRIVATE(this)->devices += new Mouse(host);
  PRIVATE(this)->devices += new Keyboard(host);

#ifdef HAVE_SPACENAV_LIB
  PRIVATE(this)->devices += new SpaceNavigatorDevice(host);
#endif // HAVE_SPACENAV_LIB

}

EventFilter::~EventFilter()
{
  qDeleteAll(PRIVATE(this)->devices);
  delete PRIVATE(this);
}

/*!
  Adds a device for event translation
 */
void 
EventFilter::registerInputDevice(InputDevice * device)
{
  PRIVATE(this)->devices += device;
}

/*!
  Removes a device from event translation
 */
void 
EventFilter::unregisterInputDevice(InputDevice * device)
{
  int i = PRIVATE(this)->devices.indexOf(device);
  if (i != -1) {
    PRIVATE(this)->devices.removeAt(i);
  }
}

/*! Translates Qt Events into Coin events and passes them on to the
  event QuarterWidget for processing. If the event cannot be
  translated or processed, it is forwarded to Qt and the method
  returns false.
 */
bool
EventFilter::eventFilter(QObject * obj, QEvent * qevent)
{
  Q_UNUSED(obj); 
  // make sure every device has updated screen size and mouse position
  // before translating events
  switch (qevent->type()) {
  case QEvent::MouseMove:
  case QEvent::MouseButtonPress:
  case QEvent::MouseButtonRelease:
  case QEvent::MouseButtonDblClick:
    PRIVATE(this)->trackPointerPosition(dynamic_cast<QMouseEvent *>(qevent));
    break;
  case QEvent::Resize:
    PRIVATE(this)->trackWindowSize(dynamic_cast<QResizeEvent *>(qevent));
    break;
  default:
    break;
  }

  const bool isMove = qevent->type() == QEvent::MouseMove;
  const int staleMs = staleHoverMoveMs();
  const QMouseEvent * me = isMove ? static_cast<QMouseEvent *>(qevent) : nullptr;

  // translate QEvent into SoEvent and see if it is handled by scene
  // graph
  Q_FOREACH(InputDevice * device, PRIVATE(this)->devices) {
    const SoEvent * soevent = device->translateEvent(qevent);
    if (!soevent) {
      continue;
    }

    // A hover pick runs the whole scene traversal synchronously, so when the
    // pointer moves faster than picks complete the queued moves are already
    // obsolete when dispatched.  Consume a stale pure-hover move without
    // picking so the queue drains to the latest position; its position is
    // remembered for the trailing pick that runs once the pointer stops.
    if (isMove && staleMs > 0 && me->buttons() == Qt::NoButton) {
      const quint64 ts = me->timestamp();
      if (ts != 0) {
        QElapsedTimer clock;
        clock.start();
        const qint64 age = clock.msecsSinceReference() - static_cast<qint64>(ts);
        // Also guards a clock-domain mismatch or a synthetic timestamp: only a
        // plausible, genuinely old age is dropped.
        if (age > staleMs && age < 3600000) {
          PRIVATE(this)->deferPendingHover(this, soevent->getPosition());
          if (hoverTimingEnabled()) {
            std::fprintf(stderr, "[HOVERT] drop age=%lldms\n",
                         static_cast<long long>(age));
          }
          return true;
        }
        if (hoverTimingEnabled()) {
          std::fprintf(stderr, "[HOVERT] keep age=%lldms\n",
                       static_cast<long long>(age));
        }
      }
    }
    // A live move supersedes any pending trailing pick.
    if (isMove) {
      PRIVATE(this)->clearPendingHover();
    }

    if (PRIVATE(this)->host->processSoEvent(soevent)) {
      return true;
    }
  }
  return false;
}

/*!
  Returns mouse position in global coordinates
 */
const QPoint &
EventFilter::globalMousePosition() const
{
  return PRIVATE(this)->globalmousepos;
}

#undef PRIVATE

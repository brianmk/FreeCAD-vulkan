// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 Joao Matos
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1            *
 *   of the License, or (at your option) any later version.                   *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful,               *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty              *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
 *   See the GNU Lesser General Public License for more details.              *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see https://www.gnu.org/licenses     *
 *                                                                            *
 ******************************************************************************/

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/Document.h"
#include "App/FeatureTest.h"
#include "App/MainThreadSignal.h"
#include <src/App/InitApplication.h>

namespace
{

bool g_onMainThread = true;
std::vector<std::function<void()>> g_pending;

bool testIsMainThread()
{
    return g_onMainThread;
}

void testInvoke(std::function<void()>&& fn, bool blocking)
{
    if (blocking) {
        fn();
        return;
    }
    g_pending.push_back(std::move(fn));
}

void drainPending()
{
    auto pending = std::move(g_pending);
    g_pending.clear();
    for (auto& fn : pending) {
        fn();
    }
}

App::MainThreadSignalConfig::ObjectHandle testEncodeObject(const App::DocumentObject* object)
{
    App::MainThreadSignalConfig::ObjectHandle handle;
    if (!object) {
        return handle;
    }
    if (const char* name = object->getNameInDocument()) {
        handle.object = name;
    }
    if (const App::Document* document = object->getDocument()) {
        handle.document = document->getName();
    }
    return handle;
}

const App::DocumentObject* testResolveObject(const App::MainThreadSignalConfig::ObjectHandle& handle)
{
    const App::Document* document = App::GetApplication().getDocument(handle.document.c_str());
    return document ? document->getObject(handle.object.c_str()) : nullptr;
}

// Installs the test main-thread hooks for the lifetime of the test and clears
// them afterwards so other tests see the default "no hooks, run inline" state.
class HookGuard
{
public:
    HookGuard()
    {
        App::MainThreadSignalConfig::setHooks(&testIsMainThread, &testInvoke);
        App::MainThreadSignalConfig::setDeferredHooks(
            nullptr,
            &testEncodeObject,
            nullptr,
            nullptr,
            &testResolveObject,
            nullptr
        );
    }

    ~HookGuard()
    {
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        App::MainThreadSignalConfig::setDeferredHooks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        g_pending.clear();
    }
};

}  // namespace

class MainThreadSignalPolicyTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

TEST_F(MainThreadSignalPolicyTest, QueuedValueIsDeliveredLater)
{
    HookGuard guard;

    App::MainThreadSignal<void(std::string), App::DeliveryPolicy::Queued> signal;
    std::string received;
    int calls = 0;
    signal.connect([&](const std::string& value) {
        received = value;
        ++calls;
    });

    g_onMainThread = false;
    signal.emit(std::string("hello"));

    EXPECT_EQ(calls, 0);  // deferred, not inline
    EXPECT_EQ(g_pending.size(), 1U);

    g_onMainThread = true;
    drainPending();

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(received, "hello");
}

TEST_F(MainThreadSignalPolicyTest, QueuedCallbackDroppedWhenSignalDestroyed)
{
    HookGuard guard;

    {
        App::MainThreadSignal<void(std::string), App::DeliveryPolicy::Queued> signal;
        signal.connect([](const std::string&) {});
        g_onMainThread = false;
        signal.emit(std::string("gone"));
    }

    // The signal is gone; running the queued callback must not touch it.
    g_onMainThread = true;
    drainPending();
    SUCCEED();
}

TEST_F(MainThreadSignalPolicyTest, CoalescedCollapsesDuplicateIdentities)
{
    HookGuard guard;

    App::MainThreadSignal<void(std::string), App::DeliveryPolicy::Coalesced> signal;
    std::vector<std::string> received;
    signal.connect([&](const std::string& value) { received.push_back(value); });

    g_onMainThread = false;
    signal.emit(std::string("a"));
    signal.emit(std::string("a"));
    signal.emit(std::string("b"));

    EXPECT_TRUE(received.empty());
    EXPECT_EQ(g_pending.size(), 1U);  // one scheduled drain

    g_onMainThread = true;
    drainPending();

    EXPECT_EQ(received.size(), 2U);  // "a" once, "b" once
}

TEST_F(MainThreadSignalPolicyTest, QueuedObjectResolvesLiveAndDropsDeleted)
{
    HookGuard guard;

    const std::string docName = App::GetApplication().getUniqueDocumentName("mts_policy");
    App::Document* doc = App::GetApplication().newDocument(docName.c_str(), "testUser");

    auto* live = doc->addObject("App::FeatureTest", "Live");
    auto* doomed = doc->addObject("App::FeatureTest", "Doomed");
    ASSERT_NE(live, nullptr);
    ASSERT_NE(doomed, nullptr);

    App::MainThreadSignal<void(const App::DocumentObject&), App::DeliveryPolicy::Coalesced> signal;
    std::vector<std::string> received;
    signal.connect([&](const App::DocumentObject& object) {
        received.emplace_back(object.getNameInDocument());
    });

    g_onMainThread = false;
    signal.emit(*live);
    signal.emit(*doomed);

    // Back on the main thread, delete one target before the deferred delivery
    // runs.  removeObject() emits blocking production signals, which is only
    // valid on the main thread.
    g_onMainThread = true;
    doc->removeObject("Doomed");
    drainPending();

    // Only the still-live object is delivered; the deleted one is dropped.
    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(received.front(), "Live");

    App::GetApplication().closeDocument(docName.c_str());
}

TEST_F(MainThreadSignalPolicyTest, PendingRemovalSweepRemovesQueuedObjects)
{
    const std::string docName = App::GetApplication().getUniqueDocumentName("mts_sweep");
    App::Document* doc = App::GetApplication().newDocument(docName.c_str(), "testUser");

    auto* doomed = doc->addObject("App::FeatureTest", "Doomed");
    ASSERT_NE(doomed, nullptr);

    // Simulate an object queued for removal while it was being recomputed.
    doomed->setStatus(App::ObjectStatus::PendingRecompute, true);
    doc->removeObject("Doomed");
    ASSERT_NE(doc->getObject("Doomed"), nullptr);

    // recompute() runs the pending-removal sweep at the end.
    doc->recompute();
    EXPECT_EQ(doc->getObject("Doomed"), nullptr);

    App::GetApplication().closeDocument(docName.c_str());
}

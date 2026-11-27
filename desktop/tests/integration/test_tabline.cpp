// Tier-2 tests for tabpages produced by a real Neovim. Tests enable ext_tabline on their own UI
// channel because production attaches keep the grid-rendered tabline.

#include "IntegrationHelpers.h"
#include "TabpageModel.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QtTest>

#include <memory>

namespace dot {

using namespace dot::test;

class TestTabline : public QObject {
    Q_OBJECT

private:
    static bool enableExtTabline(NvimConnector &conn) {
        conn.m_rpc->notify(QStringLiteral("nvim_ui_set_option"),
                           [](msgpack::packer<msgpack::sbuffer> &pk) {
            pk.pack_array(2);
            pk.pack(std::string("ext_tabline"));
            pk.pack(true);
        });
        const auto extTabline = evalSync(
            conn, QStringLiteral("get(nvim_list_uis()[0], 'ext_tabline', v:false)"), 20000);
        if(!extTabline || !extTabline->toBool()) return false;
        conn.command(QStringLiteral("redrawtabline"));
        return true;
    }

    static bool attach(NvimConnector &conn) {
        if(!startTestNvim(conn) || !conn.attachUi(80, 24) || !waitForAttach(&conn)) return false;
        return enableExtTabline(conn);
    }

    static QList<qint64> handles(const TabpageModel *model) {
        QList<qint64> result;
        result.reserve(model->rowCount());
        for(int row = 0; row < model->rowCount(); ++row)
            result.append(model->data(model->index(row), TabpageModel::HandleRole).toLongLong());
        return result;
    }

    static qint64 nvimCurrentTab(NvimConnector &conn) {
        const auto tab = evalSync(conn, QStringLiteral("nvim_get_current_tabpage()"), 5000);
        return tab ? tab->toLongLong() : 0;
    }

    static QList<qint64> nvimTabs(NvimConnector &conn) {
        QList<qint64> result;
        const auto tabs = evalSync(conn, QStringLiteral("nvim_list_tabpages()"), 5000);
        if(!tabs) return result;
        const QVariantList list = tabs->toList();
        result.reserve(list.size());
        for(const QVariant &tab: list) result.append(tab.toLongLong());
        return result;
    }

    static bool selectAndWait(NvimConnector &conn, qint64 handle) {
        QSignalSpy finishedSpy(conn.tabpages(), &TabpageModel::selectionFinished);
        conn.selectTabpage(handle);
        if(finishedSpy.isEmpty() && !finishedSpy.wait(5000)) return false;
        const QList<QVariant> args = finishedSpy.takeFirst();
        return args.at(0).toLongLong() == handle && args.at(1).toBool();
    }

    // Opens three tabs and returns their handles in Neovim's order, with the last one current.
    static QList<qint64> openThreeTabs(NvimConnector &conn) {
        conn.command(QStringLiteral("tabnew"));
        conn.command(QStringLiteral("tabnew"));
        return nvimTabs(conn);
    }

private slots:
    void init() { QTest::failOnWarning(QRegularExpression(QStringLiteral("^Invalid tabline_update"))); }

    void defaultAttachLeavesTablineOff() {
        NvimConnector conn;
        FlushLatch initialFlush(&conn);
        QVERIFY(startTestNvim(conn));
        QVERIFY(conn.attachUi(80, 24));
        QVERIFY(waitForAttach(&conn));
        QVERIFY(initialFlush.wait());

        // packAttachOptions keeps the tabline on the grid until a tab strip consumes the model.
        const auto extTabline =
            evalSync(conn, QStringLiteral("get(nvim_list_uis()[0], 'ext_tabline', v:false)"));
        QVERIFY(extTabline);
        QCOMPARE(extTabline->toBool(), false);

        QSignalSpy flushSpy(&conn, &NvimConnector::flush);
        conn.command(QStringLiteral("tabnew"));
        QVERIFY(flushSpy.wait());

        const auto tabCount = evalSync(conn, QStringLiteral("tabpagenr('$')"));
        QVERIFY(tabCount);
        QCOMPARE(tabCount->toInt(), 2);
        QCOMPARE(conn.tabpages()->rowCount(), 0);
        QCOMPARE(conn.tabpages()->currentHandle(), 0);
    }

    void selectionTargetsHandle() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        TabpageModel *model = conn.tabpages();
        const QList<qint64> tabs = openThreeTabs(conn);
        QCOMPARE(tabs.size(), 3);
        const qint64 second = tabs.at(1);
        const qint64 third = tabs.at(2);
        QTRY_COMPARE(handles(model), tabs);
        QSignalSpy movedSpy(model, &TabpageModel::rowsMoved);
        QSignalSpy resetSpy(model, &TabpageModel::modelReset);

        conn.command(QStringLiteral("tabmove 0"));

        const QList<qint64> moved{ third, tabs.at(0), second };
        QCOMPARE(nvimTabs(conn), moved);
        QTRY_COMPARE(handles(model), moved);
        QVERIFY(!movedSpy.isEmpty());
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(model->currentHandle(), third);
        QCOMPARE(nvimCurrentTab(conn), third);

        QVERIFY(selectAndWait(conn, second));

        QTRY_COMPARE(model->currentHandle(), second);
        QCOMPARE(nvimCurrentTab(conn), second);
        QCOMPARE(model->selectionError(), QString());
        QCOMPARE(model->selectionPending(), false);
    }

    void closedTargetDoesNotSelectNeighbor() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        TabpageModel *model = conn.tabpages();
        const QList<qint64> tabs = openThreeTabs(conn);
        QCOMPARE(tabs.size(), 3);
        const qint64 first = tabs.at(0);
        const qint64 second = tabs.at(1);
        const qint64 third = tabs.at(2);
        conn.command(QStringLiteral("tabfirst"));
        QTRY_COMPARE(handles(model), tabs);
        QTRY_COMPARE(model->currentHandle(), first);
        QSignalSpy finishedSpy(model, &TabpageModel::selectionFinished);

        // Close the target and select it in the same turn, so the model still lists it while
        // third moves into its former position.
        conn.command(QStringLiteral("tabclose 2"));
        conn.selectTabpage(second);

        QTRY_COMPARE(finishedSpy.count(), 1);
        const QList<QVariant> args = finishedSpy.at(0);
        QCOMPARE(args.at(0).toLongLong(), second);
        QCOMPARE(args.at(1).toBool(), false);
        QVERIFY(!args.at(2).toString().isEmpty());
        QCOMPARE(model->selectionError(), args.at(2).toString());
        QTRY_COMPARE(handles(model), (QList<qint64>{ first, third }));
        QCOMPARE(model->currentHandle(), first);
        QCOMPARE(nvimCurrentTab(conn), first);
        QCOMPARE(finishedSpy.count(), 1);
    }

    void restartRepopulatesWithNewHandles() {
        NvimConnector conn;
        QVERIFY(attach(conn));
        const auto apiLevel = evalSync(conn, QStringLiteral("api_info().version.api_level"));
        QVERIFY(apiLevel);
        if(apiLevel->toInt() < 14) QSKIP("nvim API level < 14: :restart UI event unavailable");
        const auto oldPid = evalSync(conn, QStringLiteral("getpid()"));
        QVERIFY(oldPid && oldPid->toInt() > 0);
        TabpageModel *model = conn.tabpages();
        conn.command(QStringLiteral("tabnew"));
        const QList<qint64> oldTabs = nvimTabs(conn);
        QCOMPARE(oldTabs.size(), 2);
        QTRY_COMPARE(handles(model), oldTabs);
        QSignalSpy attachSpy(&conn, &NvimConnector::attachComplete);
        QSignalSpy finishedSpy(model, &TabpageModel::selectionFinished);
        QSignalSpy resetSpy(model, &TabpageModel::modelReset);

        conn.command(QStringLiteral("restart!"));

        QTRY_COMPARE_WITH_TIMEOUT(attachSpy.count(), 1, 20000);
        const auto newPid = evalSync(conn, QStringLiteral("getpid()"), 20000);
        QVERIFY(newPid && newPid->toInt() > 0 && newPid->toInt() != oldPid->toInt());
        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(model->currentHandle(), 0);
        QVERIFY(enableExtTabline(conn));

        const QList<qint64> newTabs = nvimTabs(conn);
        QCOMPARE(newTabs.size(), 1);
        QTRY_COMPARE(handles(model), newTabs);
        QCOMPARE(model->currentHandle(), nvimCurrentTab(conn));
        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(finishedSpy.count(), 0);
    }

    void qmlConsumerSelectsByHandle() {
        NvimConnector conn;
        QQmlEngine engine;
        QVERIFY(attach(conn));
        TabpageModel *model = conn.tabpages();
        const QList<qint64> tabs = openThreeTabs(conn);
        QCOMPARE(tabs.size(), 3);
        QTRY_COMPARE(handles(model), tabs);
        const qint64 target = tabs.at(0);
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQml
import QtQml.Models
import Dot

QtObject {
    id: root

    required property NvimConnector connector
    readonly property int count: tabs.count
    property int finishedCount: 0
    property var finishedHandle
    property bool succeeded: false

    property Instantiator tabs: Instantiator {
        model: root.connector.tabpages
        delegate: QtObject {
            required property var handle
        }
    }

    property Connections selection: Connections {
        target: root.connector.tabpages

        function onSelectionFinished(handle, succeeded, error) {
            root.finishedHandle = handle
            root.succeeded = succeeded
            root.finishedCount += 1
        }
    }

    function selectRow(row) {
        root.connector.selectTabpage(root.tabs.objectAt(row).handle)
    }
}
)",
                          QUrl());

        const std::unique_ptr<QObject> root(component.createWithInitialProperties(
            { { QStringLiteral("connector"), QVariant::fromValue(&conn) } }));

        QVERIFY2(root, qPrintable(component.errorString()));
        QCOMPARE(root->property("count").toInt(), model->rowCount());
        QCOMPARE(model->currentHandle(), tabs.at(2));
        QCOMPARE(nvimCurrentTab(conn), tabs.at(2));

        QVERIFY(QMetaObject::invokeMethod(root.get(), "selectRow", Q_ARG(QVariant, 0)));

        QTRY_COMPARE(root->property("finishedCount").toInt(), 1);
        QCOMPARE(root->property("finishedHandle").toLongLong(), target);
        QCOMPARE(root->property("succeeded").toBool(), true);
        QTRY_COMPARE(model->currentHandle(), target);
        QCOMPARE(nvimCurrentTab(conn), target);
    }
};

} // namespace dot

QTEST_MAIN(dot::TestTabline)
#include "test_tabline.moc"

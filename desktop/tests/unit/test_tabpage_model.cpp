// Tier-1 tests for tabline redraw decoding and tabpage selection. Events go through the production
// NvimConnector redraw parser into its owned TabpageModel without starting Neovim.

#include <QAbstractItemModelTester>
#include <QLocalServer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QtTest>
#include <QUuid>

#include "NvimConnector.h"
#include "TabpageModel.h"

#include <msgpack.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dot {

namespace {

using Packer = msgpack::packer<msgpack::sbuffer>;

constexpr std::int8_t kBufferType = 0;
constexpr std::int8_t kTabpageType = 2;

std::string handleBody(std::int64_t id) {
    msgpack::sbuffer buffer;
    msgpack::pack(buffer, id);
    return { buffer.data(), buffer.size() };
}

struct Tab {
    std::string body;
    std::string name;
    std::int8_t type = kTabpageType;
};

Tab tab(std::int64_t id, const std::string &name) { return { handleBody(id), name }; }

void packExt(Packer &pk, std::int8_t type, const std::string &body) {
    pk.pack_ext(body.size(), type);
    pk.pack_ext_body(body.data(), static_cast<uint32_t>(body.size()));
}

void packTabs(Packer &pk, const std::vector<Tab> &tabs) {
    pk.pack_array(static_cast<uint32_t>(tabs.size()));
    for(const Tab &entry: tabs) {
        pk.pack_map(2);
        pk.pack(std::string("tab"));
        packExt(pk, entry.type, entry.body);
        pk.pack(std::string("name"));
        pk.pack(entry.name);
    }
}

void packBuffers(Packer &pk) {
    packExt(pk, kBufferType, handleBody(1));
    pk.pack_array(1);
    pk.pack_map(2);
    pk.pack(std::string("buffer"));
    packExt(pk, kBufferType, handleBody(1));
    pk.pack(std::string("name"));
    pk.pack(std::string("file.txt"));
}

QByteArray tablineBatch(const std::function<void(Packer &)> &packArguments) {
    msgpack::sbuffer buffer;
    Packer pk(buffer);
    pk.pack_array(1);
    pk.pack_array(2);
    pk.pack(std::string("tabline_update"));
    packArguments(pk);
    return QByteArray(buffer.data(), static_cast<qsizetype>(buffer.size()));
}

QByteArray tablineEvent(const std::string &currentBody, const std::vector<Tab> &tabs) {
    return tablineBatch([&](Packer &pk) {
        pk.pack_array(4);
        packExt(pk, kTabpageType, currentBody);
        packTabs(pk, tabs);
        packBuffers(pk);
    });
}

QByteArray tablineEvent(std::int64_t current, const std::vector<Tab> &tabs) {
    return tablineEvent(handleBody(current), tabs);
}

} // namespace

class TestTabpageModel : public QObject {
    Q_OBJECT

private:
    static void dispatch(NvimConnector &conn, const QByteArray &batch) {
        const msgpack::object_handle handle =
            msgpack::unpack(batch.constData(), static_cast<std::size_t>(batch.size()));
        conn.handleRedraw(handle.get());
    }

    static QList<qint64> handles(const TabpageModel *model) {
        QList<qint64> result;
        for(int row = 0; row < model->rowCount(); ++row)
            result.append(model->data(model->index(row), TabpageModel::HandleRole).toLongLong());
        return result;
    }

    static QStringList names(const TabpageModel *model) {
        QStringList result;
        for(int row = 0; row < model->rowCount(); ++row)
            result.append(model->data(model->index(row), TabpageModel::NameRole).toString());
        return result;
    }

    static bool current(const TabpageModel *model, int row) {
        return model->data(model->index(row), TabpageModel::CurrentRole).toBool();
    }

    static void verifyFinished(const QSignalSpy &spy, int index, qint64 handle, bool succeeded,
                               const QString &error) {
        QVERIFY(spy.count() > index);
        const QList<QVariant> arguments = spy.at(index);
        QCOMPARE(arguments.at(0).toLongLong(), handle);
        QCOMPARE(arguments.at(1).toBool(), succeeded);
        QCOMPARE(arguments.at(2).toString(), error);
    }

    static void triggerLifecycleReset(NvimConnector &conn, int path) {
        if(path == 0) {
            conn.onRpcDisconnected();
        } else if(path == 1) {
            conn.m_restartInProgress = true;
            conn.onRestartFailed();
        } else {
            conn.resetUiState();
        }
    }

private slots:
    void init() {
        QTest::failOnWarning(QRegularExpression(QStringLiteral("^Invalid tabline_update")));
    }

    void decodesFullEvent() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy currentSpy(model, &TabpageModel::currentHandleChanged);

        dispatch(conn, tablineBatch([](Packer &pk) {
            pk.pack_array(4);
            packExt(pk, kTabpageType, handleBody(2));
            pk.pack_array(2);
            pk.pack_map(4);
            pk.pack(std::string("modified"));
            pk.pack(true);
            pk.pack(std::string("tab"));
            packExt(pk, kTabpageType, handleBody(1));
            pk.pack(7);
            pk.pack(std::string("ignored"));
            pk.pack(std::string("name"));
            pk.pack(std::string("one"));
            pk.pack_map(2);
            pk.pack(std::string("name"));
            pk.pack(std::string("two"));
            pk.pack(std::string("tab"));
            packExt(pk, kTabpageType, handleBody(2));
            packBuffers(pk);
        }));

        QCOMPARE(handles(model), QList<qint64>({ 1, 2 }));
        QCOMPARE(names(model), QStringList({ QStringLiteral("one"), QStringLiteral("two") }));
        QCOMPARE(model->currentHandle(), 2);
        QCOMPARE(current(model, 0), false);
        QCOMPARE(current(model, 1), true);
        QCOMPARE(currentSpy.count(), 1);
    }

    void reorderMovesRowsWithoutReset() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(2, "two"), tab(3, "three") }));
        QSignalSpy insertSpy(model, &TabpageModel::rowsInserted);
        QSignalSpy moveSpy(model, &TabpageModel::rowsMoved);
        QSignalSpy removeSpy(model, &TabpageModel::rowsRemoved);
        QSignalSpy resetSpy(model, &TabpageModel::modelReset);

        dispatch(conn, tablineEvent(1, { tab(3, "three"), tab(1, "one"), tab(2, "two") }));

        QCOMPARE(handles(model), QList<qint64>({ 3, 1, 2 }));
        QCOMPARE(names(model), QStringList({ QStringLiteral("three"), QStringLiteral("one"),
                                             QStringLiteral("two") }));
        QVERIFY(!moveSpy.isEmpty());
        QCOMPARE(insertSpy.count(), 0);
        QCOMPARE(removeSpy.count(), 0);
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(model->currentHandle(), 1);
        QCOMPARE(current(model, 1), true);
    }

    void currentSwitchAndRenameEmitDataChanged() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(2, "two") }));
        QSignalSpy currentSpy(model, &TabpageModel::currentHandleChanged);
        QSignalSpy dataSpy(model, &TabpageModel::dataChanged);
        QSignalSpy insertSpy(model, &TabpageModel::rowsInserted);
        QSignalSpy moveSpy(model, &TabpageModel::rowsMoved);
        QSignalSpy removeSpy(model, &TabpageModel::rowsRemoved);
        QSignalSpy resetSpy(model, &TabpageModel::modelReset);
        QStringList order;
        connect(model, &TabpageModel::dataChanged, this,
                [&order] { order.append(QStringLiteral("data")); });
        connect(model, &TabpageModel::currentHandleChanged, this,
                [&order] { order.append(QStringLiteral("current")); });

        dispatch(conn, tablineEvent(2, { tab(1, "one"), tab(2, "two") }));

        QCOMPARE(dataSpy.count(), 2);
        for(int row = 0; row < 2; ++row) {
            QCOMPARE(dataSpy.at(row).at(0).toModelIndex(), model->index(row));
            QCOMPARE(dataSpy.at(row).at(1).toModelIndex(), model->index(row));
            QCOMPARE(dataSpy.at(row).at(2).value<QList<int>>(),
                     QList<int>{ TabpageModel::CurrentRole });
        }
        QCOMPARE(order, QStringList({ QStringLiteral("data"), QStringLiteral("data"),
                                      QStringLiteral("current") }));
        QCOMPARE(currentSpy.count(), 1);
        QCOMPARE(model->currentHandle(), 2);
        QCOMPARE(current(model, 0), false);
        QCOMPARE(current(model, 1), true);

        dataSpy.clear();
        dispatch(conn, tablineEvent(2, { tab(1, "one"), tab(2, "renamed") }));

        QCOMPARE(dataSpy.count(), 1);
        QCOMPARE(dataSpy.at(0).at(0).toModelIndex(), model->index(1));
        QCOMPARE(dataSpy.at(0).at(1).toModelIndex(), model->index(1));
        QCOMPARE(dataSpy.at(0).at(2).value<QList<int>>(), QList<int>{ TabpageModel::NameRole });
        QCOMPARE(names(model), QStringList({ QStringLiteral("one"), QStringLiteral("renamed") }));

        dataSpy.clear();
        order.clear();
        dispatch(conn, tablineEvent(2, { tab(1, "one"), tab(2, "renamed") }));

        QCOMPARE(order, QStringList());
        QCOMPARE(dataSpy.count(), 0);
        QCOMPARE(currentSpy.count(), 1);
        QCOMPARE(insertSpy.count(), 0);
        QCOMPARE(moveSpy.count(), 0);
        QCOMPARE(removeSpy.count(), 0);
        QCOMPARE(resetSpy.count(), 0);
    }

    void closeRemovesOnlyClosedRow() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(2, "two"), tab(3, "three") }));
        QSignalSpy insertSpy(model, &TabpageModel::rowsInserted);
        QSignalSpy removeSpy(model, &TabpageModel::rowsRemoved);
        QSignalSpy resetSpy(model, &TabpageModel::modelReset);

        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(3, "three") }));

        QCOMPARE(removeSpy.count(), 1);
        QCOMPARE(removeSpy.at(0).at(1).toInt(), 1);
        QCOMPARE(removeSpy.at(0).at(2).toInt(), 1);
        QCOMPARE(handles(model), QList<qint64>({ 1, 3 }));
        QCOMPARE(insertSpy.count(), 0);
        QCOMPARE(resetSpy.count(), 0);
    }

    void mixedUpdateRemovesInsertsAndMoves() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(2, "two"), tab(3, "three") }));
        QSignalSpy insertSpy(model, &TabpageModel::rowsInserted);
        QSignalSpy moveSpy(model, &TabpageModel::rowsMoved);
        QSignalSpy removeSpy(model, &TabpageModel::rowsRemoved);
        QSignalSpy resetSpy(model, &TabpageModel::modelReset);

        dispatch(conn, tablineEvent(1, { tab(4, "four"), tab(3, "three"), tab(1, "one") }));

        QCOMPARE(handles(model), QList<qint64>({ 4, 3, 1 }));
        QCOMPARE(names(model), QStringList({ QStringLiteral("four"), QStringLiteral("three"),
                                             QStringLiteral("one") }));
        QCOMPARE(insertSpy.count(), 1);
        QCOMPARE(moveSpy.count(), 1);
        QCOMPARE(removeSpy.count(), 1);
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(model->currentHandle(), 1);
        QCOMPARE(current(model, 2), true);
    }

    void pendingSelectionFailsWhenTargetClosed() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(2, "two"), tab(3, "three") }));
        const auto selection = model->beginSelection(2);
        QVERIFY(selection);
        QVERIFY(model->selectionPending());
        QSignalSpy currentSpy(model, &TabpageModel::currentHandleChanged);
        QSignalSpy finishedSpy(model, &TabpageModel::selectionFinished);

        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(3, "three") }));

        QCOMPARE(finishedSpy.count(), 1);
        verifyFinished(finishedSpy, 0, 2, false, QStringLiteral("Tabpage closed"));
        QCOMPARE(model->selectionError(), QStringLiteral("Tabpage closed"));
        QVERIFY(!model->selectionPending());
        QCOMPARE(model->currentHandle(), 1);
        QCOMPARE(currentSpy.count(), 0);

        model->finishSelection(selection->generation, {});
        QCOMPARE(finishedSpy.count(), 1);
    }

    void staleOrSupersededSelectionIgnored() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(2, "two"), tab(3, "three") }));
        QSignalSpy errorSpy(model, &TabpageModel::selectionErrorChanged);
        QSignalSpy finishedSpy(model, &TabpageModel::selectionFinished);
        QSignalSpy pendingSpy(model, &TabpageModel::selectionPendingChanged);

        const auto first = model->beginSelection(2);
        const auto second = model->beginSelection(3);

        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(first->generation != second->generation);
        QCOMPARE(finishedSpy.count(), 1);
        verifyFinished(finishedSpy, 0, 2, false, QStringLiteral("Superseded by a newer selection"));
        QCOMPARE(model->selectionError(), QStringLiteral("Superseded by a newer selection"));
        QCOMPARE(pendingSpy.count(), 1);
        QVERIFY(model->selectionPending());

        model->finishSelection(first->generation, std::unexpected(QStringLiteral("late")));
        QCOMPARE(finishedSpy.count(), 1);
        QVERIFY(model->selectionPending());

        model->finishSelection(second->generation, {});
        QCOMPARE(finishedSpy.count(), 2);
        verifyFinished(finishedSpy, 1, 3, true, QString());
        QCOMPARE(model->selectionError(), QString());
        QCOMPARE(errorSpy.count(), 2);
        QVERIFY(!model->selectionPending());
        QCOMPARE(pendingSpy.count(), 2);
        QCOMPARE(model->currentHandle(), 1);
    }

    void unknownHandleFailsWithoutRequest() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(2, "two") }));
        QSignalSpy finishedSpy(model, &TabpageModel::selectionFinished);
        QSignalSpy pendingSpy(model, &TabpageModel::selectionPendingChanged);

        conn.selectTabpage(99);

        QCOMPARE(finishedSpy.count(), 1);
        verifyFinished(finishedSpy, 0, 99, false, QStringLiteral("Unknown tabpage"));
        QCOMPARE(model->selectionError(), QStringLiteral("Unknown tabpage"));
        QVERIFY(!model->selectionPending());
        QCOMPARE(pendingSpy.count(), 0);
    }

    void beginSelectionClearsPreviousError() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(2, "two") }));
        conn.selectTabpage(99);
        QCOMPARE(model->selectionError(), QStringLiteral("Unknown tabpage"));
        QSignalSpy errorSpy(model, &TabpageModel::selectionErrorChanged);

        QVERIFY(model->beginSelection(2));

        QVERIFY(model->selectionError().isEmpty());
        QVERIFY(model->selectionPending());
        QCOMPARE(errorSpy.count(), 1);
    }

    void selectWithoutTransportReportsError() {
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(2, { tab(1, "one"), tab(2, "two") }));
        QSignalSpy finishedSpy(model, &TabpageModel::selectionFinished);
        QSignalSpy pendingSpy(model, &TabpageModel::selectionPendingChanged);

        conn.selectTabpage(1);

        QCOMPARE(finishedSpy.count(), 1);
        verifyFinished(finishedSpy, 0, 1, false, QStringLiteral("nvim not running"));
        QCOMPARE(model->selectionError(), QStringLiteral("nvim not running"));
        QVERIFY(!model->selectionPending());
        QCOMPARE(pendingSpy.count(), 2);
        QCOMPARE(model->currentHandle(), 2);
    }

    void lifecycleResetFailsPendingSelection_data() {
        QTest::addColumn<int>("path");
        QTest::addColumn<QString>("reason");
        QTest::newRow("disconnect") << 0 << QStringLiteral("Neovim disconnected");
        QTest::newRow("restart failed") << 1 << QStringLiteral("Neovim restart failed");
        QTest::newRow("ui state reset") << 2 << QStringLiteral("Neovim restarted");
    }

    void lifecycleResetFailsPendingSelection() {
        QFETCH(int, path);
        QFETCH(QString, reason);
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy currentSpy(model, &TabpageModel::currentHandleChanged);
        QSignalSpy errorSpy(model, &TabpageModel::selectionErrorChanged);
        QSignalSpy finishedSpy(model, &TabpageModel::selectionFinished);
        QSignalSpy pendingSpy(model, &TabpageModel::selectionPendingChanged);
        QSignalSpy resetSpy(model, &TabpageModel::modelReset);

        triggerLifecycleReset(conn, path);

        QCOMPARE(currentSpy.count(), 0);
        QCOMPARE(errorSpy.count(), 0);
        QCOMPARE(finishedSpy.count(), 0);
        QCOMPARE(pendingSpy.count(), 0);
        QCOMPARE(resetSpy.count(), 0);

        dispatch(conn, tablineEvent(2, { tab(1, "one"), tab(2, "two") }));
        QVERIFY(model->beginSelection(1));
        currentSpy.clear();
        pendingSpy.clear();

        triggerLifecycleReset(conn, path);

        QCOMPARE(finishedSpy.count(), 1);
        verifyFinished(finishedSpy, 0, 1, false, reason);
        QCOMPARE(model->selectionError(), reason);
        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(model->currentHandle(), 0);
        QCOMPARE(currentSpy.count(), 1);
        QVERIFY(!model->selectionPending());
        QCOMPARE(pendingSpy.count(), 1);
    }

    void lifecycleResetClearsStaleError_data() { lifecycleResetFailsPendingSelection_data(); }

    void lifecycleResetClearsStaleError() {
        QFETCH(int, path);
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(1, { tab(1, "one") }));
        conn.selectTabpage(99);
        QCOMPARE(model->selectionError(), QStringLiteral("Unknown tabpage"));
        QSignalSpy errorSpy(model, &TabpageModel::selectionErrorChanged);
        QSignalSpy finishedSpy(model, &TabpageModel::selectionFinished);

        triggerLifecycleReset(conn, path);

        QVERIFY(model->selectionError().isEmpty());
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(finishedSpy.count(), 0);
    }

    void restartInvalidatesPendingSelection() {
        const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QLocalServer server;
        QVERIFY(server.listen(QStringLiteral("dot-test-tabpage-%1").arg(suffix)));
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        conn.m_rpc->connectToAddress(server.fullServerName());
        QTRY_VERIFY(conn.m_rpc->isRunning());
        dispatch(conn, tablineEvent(1, { tab(1, "one"), tab(2, "two") }));
        QSignalSpy finishedSpy(model, &TabpageModel::selectionFinished);

        conn.selectTabpage(2);
        QVERIFY(model->selectionPending());
        QCOMPARE(finishedSpy.count(), 0);

        conn.m_restartPending = true;
        conn.m_restartListenAddr = QStringLiteral("dot-test-tabpage-missing-%1").arg(suffix);
        conn.onRpcDisconnected();

        QCOMPARE(finishedSpy.count(), 1);
        verifyFinished(finishedSpy, 0, 2, false, QStringLiteral("Neovim restarted"));
        QVERIFY(!model->selectionPending());
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 0, 10000);
        QCOMPARE(finishedSpy.count(), 1);
    }

    void malformedEventsKeepLastValidState_data() {
        QTest::addColumn<QByteArray>("batch");
        QTest::addColumn<QString>("warning");

        const auto withArguments = [](const std::function<void(Packer &)> &packArguments) {
            return tablineBatch(packArguments);
        };
        const auto withCurtabBody = [](const std::string &body) {
            return tablineEvent(body, { tab(1, "one") });
        };
        const auto withTabs = [](const std::function<void(Packer &)> &packTabsArgument) {
            return tablineBatch([&packTabsArgument](Packer &pk) {
                pk.pack_array(2);
                packExt(pk, kTabpageType, handleBody(1));
                packTabsArgument(pk);
            });
        };
        const QString curtab =
            QStringLiteral("Invalid tabline_update: curtab must be a tabpage handle");

        QTest::newRow("argument not array")
            << withArguments([](Packer &pk) { pk.pack(1); })
            << QStringLiteral("Invalid tabline_update: expected an argument array");
        QTest::newRow("too few arguments") << withArguments([](Packer &pk) {
            pk.pack_array(1);
            packExt(pk, kTabpageType, handleBody(1));
        }) << QStringLiteral("Invalid tabline_update: expected curtab and tabs");
        QTest::newRow("curtab not ext") << withArguments([](Packer &pk) {
            pk.pack_array(2);
            pk.pack(1);
            packTabs(pk, { tab(1, "one") });
        }) << curtab;
        QTest::newRow("empty ext body") << withCurtabBody(std::string()) << curtab;
        QTest::newRow("truncated ext body") << withCurtabBody(std::string("\xcd\x01", 2)) << curtab;
        QTest::newRow("non-integer ext body") << withCurtabBody(std::string("\xa1x", 2)) << curtab;
        QTest::newRow("negative ext body") << withCurtabBody(handleBody(-1)) << curtab;
        QTest::newRow("ext body above qint64") << withCurtabBody([] {
            msgpack::sbuffer buffer;
            msgpack::pack(buffer, std::uint64_t{ 1 } << 63);
            return std::string(buffer.data(), buffer.size());
        }()) << curtab;
        QTest::newRow("trailing ext byte") << withCurtabBody(std::string("\x01\x00", 2)) << curtab;
        QTest::newRow("tabs not array")
            << withTabs([](Packer &pk) { pk.pack(1); })
            << QStringLiteral("Invalid tabline_update: tabs must be an array");
        QTest::newRow("tab entry not map") << withTabs([](Packer &pk) {
            pk.pack_array(1);
            pk.pack(1);
        }) << QStringLiteral("Invalid tabline_update: tab entry must be a map");
        QTest::newRow("tab entry without handle") << withTabs([](Packer &pk) {
            pk.pack_array(1);
            pk.pack_map(1);
            pk.pack(std::string("name"));
            pk.pack(std::string("one"));
        }) << QStringLiteral("Invalid tabline_update: tab entry needs a tabpage handle");
        QTest::newRow("tab handle type mismatch") << withTabs([](Packer &pk) {
            packTabs(pk, { Tab{ handleBody(1), "one", kBufferType } });
        }) << QStringLiteral("Invalid tabline_update: tab handle type differs from curtab");
        QTest::newRow("tab name not string") << withTabs([](Packer &pk) {
            pk.pack_array(1);
            pk.pack_map(2);
            pk.pack(std::string("tab"));
            packExt(pk, kTabpageType, handleBody(1));
            pk.pack(std::string("name"));
            pk.pack(1);
        }) << QStringLiteral("Invalid tabline_update: tab entry needs a string name");
        QTest::newRow("duplicate handle")
            << tablineEvent(1, { tab(1, "one"), tab(1, "again") })
            << QStringLiteral("Invalid tabline_update: duplicate tabpage 1");
        QTest::newRow("curtab not in tabs")
            << tablineEvent(3, { tab(1, "one"), tab(2, "two") })
            << QStringLiteral("Invalid tabline_update: current tabpage 3 is not in tabs");
    }

    void malformedEventsKeepLastValidState() {
        QFETCH(QByteArray, batch);
        QFETCH(QString, warning);
        NvimConnector conn;
        TabpageModel *model = conn.tabpages();
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        dispatch(conn, tablineEvent(2, { tab(1, "one"), tab(2, "two") }));
        QSignalSpy currentSpy(model, &TabpageModel::currentHandleChanged);
        QSignalSpy dataSpy(model, &TabpageModel::dataChanged);
        QSignalSpy insertSpy(model, &TabpageModel::rowsInserted);
        QSignalSpy moveSpy(model, &TabpageModel::rowsMoved);
        QSignalSpy removeSpy(model, &TabpageModel::rowsRemoved);
        QSignalSpy resetSpy(model, &TabpageModel::modelReset);

        QTest::ignoreMessage(QtWarningMsg, warning.toUtf8().constData());
        dispatch(conn, batch);

        QCOMPARE(handles(model), QList<qint64>({ 1, 2 }));
        QCOMPARE(names(model), QStringList({ QStringLiteral("one"), QStringLiteral("two") }));
        QCOMPARE(model->currentHandle(), 2);
        QCOMPARE(currentSpy.count(), 0);
        QCOMPARE(dataSpy.count(), 0);
        QCOMPARE(insertSpy.count(), 0);
        QCOMPARE(moveSpy.count(), 0);
        QCOMPARE(removeSpy.count(), 0);
        QCOMPARE(resetSpy.count(), 0);
    }
};

} // namespace dot

QTEST_GUILESS_MAIN(dot::TestTabpageModel)
#include "test_tabpage_model.moc"

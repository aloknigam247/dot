#include <QSignalSpy>
#include <QtTest>

#include "ChatModel.h"

using namespace dot;

class TestChatModel : public QObject {
    Q_OBJECT
private slots:
    void appendBlockInsertsRowWithAuthorAndText_data() {
        QTest::addColumn<QString>("author");
        QTest::newRow("assistant") << QStringLiteral("assistant");
        QTest::newRow("system") << QStringLiteral("system");
        QTest::newRow("user") << QStringLiteral("user");
    }

    void appendBlockInsertsRowWithAuthorAndText() {
        QFETCH(QString, author);
        ChatModel m;
        QSignalSpy countChanged(&m, &ChatModel::countChanged);
        QSignalSpy inserted(&m, &QAbstractItemModel::rowsInserted);
        m.appendBlock(author, QStringLiteral("hi"));
        QCOMPARE(m.count(), 1);
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.authorAt(0), author);
        QCOMPARE(m.textAt(0), QStringLiteral("hi"));
        QCOMPARE(m.data(m.index(0, 0), ChatModel::AuthorRole).toString(), author);
        QCOMPARE(m.data(m.index(0, 0), ChatModel::TextRole).toString(), QStringLiteral("hi"));
        QCOMPARE(countChanged.count(), 1);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(0).value<QModelIndex>(), QModelIndex());
        QCOMPARE(inserted.at(0).at(1).toInt(), 0);
        QCOMPARE(inserted.at(0).at(2).toInt(), 0);
    }

    void appendBlockSystemAuthor() {
        ChatModel m;
        m.appendBlock(QStringLiteral("system"), QStringLiteral("note"));
        QCOMPARE(m.count(), 1);
        QCOMPARE(m.authorAt(0), QStringLiteral("system"));
    }

    void appendBlockUnknownAuthorFallsBackToSystem() {
        ChatModel m;
        m.appendBlock(QStringLiteral("robot"), QStringLiteral("beep"));
        QCOMPARE(m.authorAt(0), QStringLiteral("system"));
    }

    void appendBlockEmptyTextIsIgnored() {
        ChatModel m;
        QSignalSpy countChanged(&m, &ChatModel::countChanged);
        QSignalSpy inserted(&m, &QAbstractItemModel::rowsInserted);
        m.appendBlock(QStringLiteral("user"), QString());
        QCOMPARE(m.count(), 0);
        QCOMPARE(countChanged.count(), 0);
        QCOMPARE(inserted.count(), 0);
    }

    void dataReturnsInvalidForUnknownRoleAndOutOfRange() {
        ChatModel m;
        m.appendBlock(QStringLiteral("user"), QStringLiteral("x"));
        const QModelIndex idx = m.index(0, 0);
        QVERIFY(!m.data(idx, Qt::UserRole + 99).isValid());
        QVERIFY(!m.data(m.index(5, 0), ChatModel::TextRole).isValid());
    }

    void roleNamesExposeAuthorAndText() {
        ChatModel m;
        const auto roles = m.roleNames();
        QCOMPARE(roles.value(ChatModel::AuthorRole), QByteArrayLiteral("author"));
        QCOMPARE(roles.value(ChatModel::TextRole), QByteArrayLiteral("text"));
    }
};

QTEST_GUILESS_MAIN(TestChatModel)
#include "test_chat_model.moc"

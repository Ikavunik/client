/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QtTest>
#include "syncenginetestutils.h"
#include <syncengine.h>

using namespace OCC;

SyncFileItemPtr findItem(const QSignalSpy &spy, const QString &path)
{
    for (const QList<QVariant> &args : spy) {
        auto item = args[0].value<SyncFileItemPtr>();
        if (item->destination() == path)
            return item;
    }
    return SyncFileItemPtr(new SyncFileItem);
}

bool itemSuccessful(const QSignalSpy &spy, const QString &path, const csync_instructions_e instr)
{
    auto item = findItem(spy, path);
    return item->_status == SyncFileItem::Success && item->_instruction == instr;
}

bool itemConflict(const QSignalSpy &spy, const QString &path)
{
    auto item = findItem(spy, path);
    return item->_status == SyncFileItem::Conflict && item->_instruction == CSYNC_INSTRUCTION_CONFLICT;
}

bool itemSuccessfulMove(const QSignalSpy &spy, const QString &path)
{
    return itemSuccessful(spy, path, CSYNC_INSTRUCTION_RENAME);
}

QStringList findConflicts(const FileInfo &dir)
{
    QStringList conflicts;
    for (const auto &item : dir.children) {
        if (item.name.contains("conflict")) {
            conflicts.append(item.path());
        }
    }
    return conflicts;
}

bool expectAndWipeConflict(FileModifier &local, FileInfo state, const QString path)
{
    PathComponents pathComponents(path);
    auto base = state.find(pathComponents.parentDirComponents());
    if (!base)
        return false;
    for (const auto &item : base->children) {
        if (item.name.startsWith(pathComponents.fileName()) && item.name.contains("_conflict")) {
            local.remove(item.path());
            return true;
        }
    }
    return false;
}

class TestSyncConflict : public QObject
{
    Q_OBJECT

private slots:
    void testNoUpload()
    {
        FakeFolder fakeFolder{ FileInfo::A12_B12_C12_S12() };
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.localModifier().setContents("A/a1", 'L');
        fakeFolder.remoteModifier().setContents("A/a1", 'R');
        fakeFolder.localModifier().appendByte("A/a2");
        fakeFolder.remoteModifier().appendByte("A/a2");
        fakeFolder.remoteModifier().appendByte("A/a2");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(expectAndWipeConflict(fakeFolder.localModifier(), fakeFolder.currentLocalState(), "A/a1"));
        QVERIFY(expectAndWipeConflict(fakeFolder.localModifier(), fakeFolder.currentLocalState(), "A/a2"));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testUploadAfterDownload()
    {
        FakeFolder fakeFolder{ FileInfo::A12_B12_C12_S12() };
        fakeFolder.syncEngine().account()->setCapabilities({ { "uploadConflictFiles", true } });
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        QMap<QByteArray, QString> conflictMap;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request) -> QNetworkReply * {
            if (op == QNetworkAccessManager::PutOperation) {
                auto baseFile = request.rawHeader("OC-ConflictFileFor");
                if (!baseFile.isEmpty()) {
                    auto components = request.url().toString().split('/');
                    QString conflictFile = components.mid(components.size() - 2).join('/');
                    conflictMap[baseFile] = conflictFile;
                }
            }
            return nullptr;
        });

        fakeFolder.localModifier().setContents("A/a1", 'L');
        fakeFolder.remoteModifier().setContents("A/a1", 'R');
        fakeFolder.localModifier().appendByte("A/a2");
        fakeFolder.remoteModifier().appendByte("A/a2");
        fakeFolder.remoteModifier().appendByte("A/a2");
        QVERIFY(fakeFolder.syncOnce());
        auto local = fakeFolder.currentLocalState();
        auto remote = fakeFolder.currentRemoteState();
        QCOMPARE(local, remote);

        QVERIFY(conflictMap.contains("A/a1"));
        QVERIFY(conflictMap.contains("A/a2"));
        QCOMPARE(conflictMap.size(), 2);
        QCOMPARE(Utility::conflictFileBaseName(conflictMap["A/a1"].toUtf8()), QByteArray("A/a1"));

        QCOMPARE(remote.find(conflictMap["A/a1"])->contentChar, 'L');
        QCOMPARE(remote.find("A/a1")->contentChar, 'R');

        QCOMPARE(remote.find(conflictMap["A/a2"])->size, 5);
        QCOMPARE(remote.find("A/a2")->size, 6);
    }

    void testSeparateUpload()
    {
        FakeFolder fakeFolder{ FileInfo::A12_B12_C12_S12() };
        fakeFolder.syncEngine().account()->setCapabilities({ { "uploadConflictFiles", true } });
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        QMap<QByteArray, QString> conflictMap;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request) -> QNetworkReply * {
            if (op == QNetworkAccessManager::PutOperation) {
                auto baseFile = request.rawHeader("OC-ConflictFileFor");
                if (!baseFile.isEmpty()) {
                    auto components = request.url().toString().split('/');
                    QString conflictFile = components.mid(components.size() - 2).join('/');
                    conflictMap[baseFile] = conflictFile;
                }
            }
            return nullptr;
        });

        // Explicitly add a conflict file to simulate the case where the upload of the
        // file didn't finish in the same sync run that the conflict was created.
        fakeFolder.localModifier().insert("A/a1_conflict_me-1234", 64, 'L');
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(conflictMap.size(), 1);
        QCOMPARE(conflictMap["A/a1"], QLatin1String("A/a1_conflict_me-1234"));
        QCOMPARE(fakeFolder.currentRemoteState().find(conflictMap["A/a1"])->contentChar, 'L');
        conflictMap.clear();

        // Now the user can locally alter the conflict file and it will be uploaded
        // as usual.
        fakeFolder.localModifier().setContents("A/a1_conflict_me-1234", 'P');
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(conflictMap.size(), 1);
        QCOMPARE(conflictMap["A/a1"], QLatin1String("A/a1_conflict_me-1234"));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        conflictMap.clear();

        // Similarly, remote modifications of conflict files get propagated downwards
        fakeFolder.remoteModifier().setContents("A/a1_conflict_me-1234", 'Q');
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QVERIFY(conflictMap.isEmpty());

        // Conflict files for conflict files!
        fakeFolder.remoteModifier().appendByte("A/a1_conflict_me-1234");
        fakeFolder.remoteModifier().appendByte("A/a1_conflict_me-1234");
        fakeFolder.localModifier().appendByte("A/a1_conflict_me-1234");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(conflictMap.size(), 1);
        QVERIFY(conflictMap.contains("A/a1_conflict_me-1234"));
        QCOMPARE(fakeFolder.currentRemoteState().find("A/a1_conflict_me-1234")->size, 66);
        QCOMPARE(fakeFolder.currentRemoteState().find(conflictMap["A/a1_conflict_me-1234"])->size, 65);
        conflictMap.clear();
    }

    void testConflictFileBaseName_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("output");

        QTest::newRow("")
            << "a/b/foo"
            << "";
        QTest::newRow("")
            << "a/b/foo.txt"
            << "";
        QTest::newRow("")
            << "a/b/foo_conflict"
            << "";
        QTest::newRow("")
            << "a/b/foo_conflict.txt"
            << "";

        QTest::newRow("")
            << "a/b/foo_conflict-123.txt"
            << "a/b/foo.txt";
        QTest::newRow("")
            << "a/b/foo_conflict_123.txt"
            << "a/b/foo.txt";
        QTest::newRow("")
            << "a/b/foo_conflict_foo-123.txt"
            << "a/b/foo.txt";

        QTest::newRow("")
            << "a/b/foo_conflict-123"
            << "a/b/foo";
        QTest::newRow("")
            << "a/b/foo_conflict_123"
            << "a/b/foo";
        QTest::newRow("")
            << "a/b/foo_conflict_foo-123"
            << "a/b/foo";

        // double conflict files
        QTest::newRow("")
            << "a/b/foo_conflict-123_conflict-456.txt"
            << "a/b/foo_conflict-123.txt";
        QTest::newRow("")
            << "a/b/foo_conflict_123_conflict_456.txt"
            << "a/b/foo_conflict_123.txt";
        QTest::newRow("")
            << "a/b/foo_conflict_foo-123_conflict_bar-456.txt"
            << "a/b/foo_conflict_foo-123.txt";
    }

    void testConflictFileBaseName()
    {
        QFETCH(QString, input);
        QFETCH(QString, output);
        QCOMPARE(Utility::conflictFileBaseName(input.toUtf8()), output.toUtf8());
    }
};

QTEST_GUILESS_MAIN(TestSyncConflict)
#include "testsyncconflict.moc"

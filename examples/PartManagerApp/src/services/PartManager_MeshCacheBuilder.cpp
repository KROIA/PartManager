#include "services/PartManager_MeshCacheBuilder.h"

#include "database/PartManager_DatabaseHandle.h"
#include "filestore/PartManager_FileStore.h"
#include "model3d/PartManager_Model3DFormat.h"
#include "model3d/PartManager_StepColors.h"
#include "model3d/PartManager_StepConverter.h"
#include "persistence/PartManager_PartRepository.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QTemporaryFile>
#include <QTimer>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{
	namespace
	{
		// A conversion that has not exited by now is not going to. FreeCAD tessellating a
		// component-sized STEP file is a second or two; two minutes means it is stuck on a
		// malformed file or waiting on something, and one stuck model must not stall the queue
		// behind it for the rest of the session.
		constexpr int ConversionTimeoutMs = 120000;
	}

	MeshCacheBuilder::MeshCacheBuilder(DatabaseHandle* handle, QObject* parent)
		: QObject(parent)
		, m_handle(handle)
	{
		if (m_handle != nullptr)
		{
			m_cacheRoot = QString::fromStdString(m_handle->filestorePath())
				+ QStringLiteral("/meshcache");
		}
	}

	MeshCacheBuilder::~MeshCacheBuilder()
	{
		if (m_process != nullptr)
		{
			// The subprocess outliving the app would keep writing into a database folder nothing
			// owns any more. Its temporary file goes with it; the cache entry was never renamed.
			m_process->kill();
			m_process->waitForFinished(2000);
		}
		if (!m_currentTemp.isEmpty()) { QFile::remove(m_currentTemp); }
		if (!m_currentScript.isEmpty()) { QFile::remove(m_currentScript); }
	}

	bool MeshCacheBuilder::converterAvailable()
	{
		return StepConverter::isAvailable();
	}

	int MeshCacheBuilder::pending() const
	{
		return m_queue.size() + (m_process != nullptr ? 1 : 0);
	}

	QString MeshCacheBuilder::meshFor(const QString& stepPath) const
	{
		if (m_cacheRoot.isEmpty() || stepPath.isEmpty())
		{
			return QString();
		}
		if (!StepConverter::isCacheValid(m_cacheRoot.toStdString(), stepPath.toStdString()))
		{
			return QString();
		}
		return QString::fromStdString(StepConverter::cachedMeshPath(
			m_cacheRoot.toStdString(), stepPath.toStdString()));
	}

	void MeshCacheBuilder::rescan()
	{
		if (m_cacheRoot.isEmpty() || !converterAvailable())
		{
			return;
		}

		QStringList found;
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle != nullptr && m_handle->isOpen())
		{
			const FileStore store(m_handle->filestorePath());
			for (const PartFile& file :
				PartRepository::listFilesWithRole(m_handle->connection(), PartFileRole::Kicad3DModel))
			{
				// Only STEP needs converting. An attached STL or OBJ is already a mesh and the
				// viewer draws it straight from the filestore.
				if (model3DFormatOf(file.originalFilename) != Model3DFormat::Step)
				{
					continue;
				}
				const std::string absolute = store.absolutePath(file.relativePath);
				if (absolute.empty())
				{
					continue;   // recorded but gone from disk; the viewer says so when asked
				}
				found.append(QString::fromStdString(absolute));
			}
		}
#endif

		int added = 0;
		for (const QString& path : found)
		{
			if (path == m_currentSource || m_queue.contains(path))
			{
				continue;
			}
			if (!meshFor(path).isEmpty())
			{
				continue;   // already converted, and for this exact content
			}
			m_queue.append(path);
			++added;
		}
		if (added == 0)
		{
			return;
		}

		// A run's total counts what is still outstanding, so a rescan part-way through does not
		// make the progress message jump backwards.
		m_total = m_done + pending();
		emit progressed(m_done, m_total);
		startNext();
	}

	void MeshCacheBuilder::prioritise(const QString& stepPath)
	{
		if (stepPath.isEmpty() || m_cacheRoot.isEmpty() || !converterAvailable())
		{
			return;
		}
		if (stepPath == m_currentSource || !meshFor(stepPath).isEmpty())
		{
			return;   // running, or already done
		}
		m_queue.removeAll(stepPath);
		m_queue.prepend(stepPath);
		m_total = m_done + pending();
		emit progressed(m_done, m_total);
		startNext();
	}

	void MeshCacheBuilder::startNext()
	{
		if (m_process != nullptr)
		{
			return;   // one at a time; this is called again when it finishes
		}
		if (m_queue.isEmpty())
		{
			// Idle resets the run, so the next batch counts from zero rather than from wherever
			// the last one left off.
			m_done = 0;
			m_total = 0;
			emit progressed(0, 0);
			return;
		}

		m_currentSource = m_queue.takeFirst();
		if (!QFileInfo(m_currentSource).isFile())
		{
			// Removed between the scan and now. Not a failure worth reporting to the user.
			m_currentSource.clear();
			QTimer::singleShot(0, this, &MeshCacheBuilder::startNext);
			return;
		}

		QDir().mkpath(m_cacheRoot);
		m_currentTarget = QString::fromStdString(StepConverter::cachedMeshPath(
			m_cacheRoot.toStdString(), m_currentSource.toStdString()));
		// The meshes are written straight to their final names; only the manifest goes through a
		// temporary. The manifest is what the cache is keyed on, so until it appears the stray
		// meshes are unreachable — a conversion killed halfway leaves litter, never a cache hit.
		m_currentTemp = m_currentTarget + QStringLiteral(".part");
		QFile::remove(m_currentTemp);

		QTemporaryFile script(QDir::tempPath() + QStringLiteral("/PartManager_step2stl_XXXXXX.py"));
		script.setAutoRemove(false);
		if (!script.open())
		{
			const QString source = m_currentSource;
			m_currentSource.clear();
			emit meshFailed(source, tr("Could not write the conversion script to a temporary file."));
			QTimer::singleShot(0, this, &MeshCacheBuilder::startNext);
			return;
		}
		m_currentScript = script.fileName();
		// The meshes are named from the *final* manifest, not the temporary one, so they do not
		// have to be renamed alongside it.
		script.write(QByteArray::fromStdString(StepConverter::conversionScript(
			m_currentSource.toStdString(), m_currentTemp.toStdString(),
			QFileInfo(m_currentTarget).path().toStdString() + "/"
				+ QFileInfo(m_currentTarget).completeBaseName().toStdString())));
		script.close();

		m_process = new QProcess(this);
		connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
			[this](int exitCode, QProcess::ExitStatus status)
			{
				if (m_process == nullptr)
				{
					// A crash emits errorOccurred *before* finished, so by here the failure has
					// already been dealt with and the process handed to deleteLater().
					return;
				}
				const QString stderrText = QString::fromUtf8(m_process->readAllStandardError());
				const bool ok = status == QProcess::NormalExit && exitCode == 0
					&& QFileInfo(m_currentTemp).size() > 0;
				finishCurrent(ok, stderrText.trimmed().isEmpty()
					? tr("The converter exited with code %1.").arg(exitCode)
					: stderrText.trimmed());
			});
		connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError)
			{
				// finished() does not follow a failure to start, so this is the only chance to
				// unblock the queue.
				if (m_process != nullptr && m_process->state() == QProcess::NotRunning)
				{
					finishCurrent(false, m_process->errorString());
				}
			});
		// A model that hangs the converter must not hang every model behind it.
		QTimer::singleShot(ConversionTimeoutMs, this, [this, source = m_currentSource]()
			{
				if (m_process != nullptr && m_currentSource == source)
				{
					m_process->kill();   // finished() follows and the queue moves on
				}
			});

		m_process->start(QString::fromStdString(StepConverter::converterPath()),
			QStringList() << m_currentScript);
	}

	bool MeshCacheBuilder::writeMeshSet(const QString& stepPath, const QString& geometryManifest,
		const QString& target)
	{
		QFile manifestFile(geometryManifest);
		if (!manifestFile.open(QIODevice::ReadOnly))
		{
			return false;
		}
		const std::vector<StepConverter::MeshPart> parts = StepConverter::parseGeometryManifest(
			QString::fromUtf8(manifestFile.readAll()).toStdString());
		manifestFile.close();
		if (parts.empty())
		{
			return false;
		}

		// The colours the STEP itself gives its solids. Read from the file's text because
		// FreeCAD's colour-aware reader is GUI-only and the console one drops colour entirely —
		// so by the time the meshes exist, the only place the colours still are is the source.
		StepStyles styles;
		QFile stepFile(stepPath);
		if (stepFile.open(QIODevice::ReadOnly))
		{
			styles = stepStylesOf(QString::fromLatin1(stepFile.readAll()).toStdString());
			stepFile.close();
		}

		std::vector<MeshSolidBox> boxes;
		boxes.reserve(parts.size());
		for (const StepConverter::MeshPart& part : parts)
		{
			boxes.push_back(part.box);
		}
		const std::vector<StepColor> colors = colorsForSolids(styles, boxes);

		// Written whole and renamed, so a reader never sees half a manifest. Until it lands the
		// meshes beside it are unreachable, which is what makes the litter harmless.
		QFile out(geometryManifest + QStringLiteral(".set"));
		if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			return false;
		}
		out.write(QByteArray::fromStdString(StepConverter::meshSetManifest(parts, colors)));
		out.close();

		QFile::remove(target);
		if (!QFile::rename(out.fileName(), target))
		{
			QFile::remove(out.fileName());
			return false;
		}
		QFile::remove(geometryManifest);
		return true;
	}

	void MeshCacheBuilder::finishCurrent(bool ok, const QString& reason)
	{
		if (m_currentSource.isEmpty())
		{
			return;   // already finished; see the crash ordering note on the finished() handler
		}
		const QString source = m_currentSource;
		const QString target = m_currentTarget;
		const QString temp = m_currentTemp;

		QFile::remove(m_currentScript);
		m_currentScript.clear();
		m_currentSource.clear();
		m_currentTarget.clear();
		m_currentTemp.clear();
		if (m_process != nullptr)
		{
			m_process->deleteLater();
			m_process = nullptr;
		}

		if (ok && writeMeshSet(source, temp, target))
		{
			++m_done;
			emit progressed(m_done, m_total);
			emit meshReady(source, target);
			startNext();
			return;
		}
		QFile::remove(temp);
		++m_done;
		emit progressed(m_done, m_total);
		emit meshFailed(source, reason);
		startNext();
	}

}

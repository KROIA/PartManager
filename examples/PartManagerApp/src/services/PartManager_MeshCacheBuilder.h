// @file PartManager_MeshCacheBuilder.h
// @brief Keeps every part's STEP model tessellated into a drawable mesh, in the background (§13).
//
// **One converter for the whole app.** Tessellating a STEP file means running
// FreeCAD as a subprocess for a second or two; two of them racing to write the
// same cache entry is how a half-written STL ends up being the one loaded. Every
// viewer therefore asks this object instead of starting its own process, and it
// runs exactly one conversion at a time.
//
// The queue fills itself: `rescan()` walks every part that has a 3D model,
// keeps the STEP files whose mesh is missing or out of date, and works through
// them while the user does something else — so by the time a part is selected
// its mesh is usually already there. `prioritise()` jumps one to the front, for
// the model the user is actually looking at right now.
//
// Cache entries are named after a hash of the STEP file's *contents*
// (StepConverter::cachedMeshPath), so a replaced model converts again and two
// parts sharing one model convert once. Nothing here consults a timestamp.
//
// Conversion writes to a sibling `.tmp.stl` and renames on success: a crash or a
// closed app leaves a temporary file, never a truncated cache entry that would
// look valid forever.
// @see docs/design/ARCHITECTURE.md §13
// @see PartManager_StepConverter.h, PartManager_Model3DViewer.h
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

namespace PartManager
{

	class DatabaseHandle;

	class MeshCacheBuilder : public QObject
	{
		Q_OBJECT
	public:
		// `handle` may be null, in which case the builder is inert — it is the same object either
		// way so callers need no null checks of their own.
		explicit MeshCacheBuilder(DatabaseHandle* handle, QObject* parent = nullptr);
		~MeshCacheBuilder() override;

		// Where meshes are cached: `<filestore>/meshcache`. Derived data, so it lives with the
		// database folder and goes when that goes.
		const QString& cacheRoot() const { return m_cacheRoot; }

		// The mesh for `stepPath` if it is already converted, otherwise empty.
		QString meshFor(const QString& stepPath) const;

		// False when no converter is installed — the viewer then explains that instead of waiting
		// for a conversion that will never start.
		static bool converterAvailable();

		// How many models are still waiting, including the one being converted.
		int pending() const;

	public slots:
		// Walks every part's 3D model and queues the STEP files that have no current mesh. Cheap
		// to call again: an up-to-date model is skipped, and one already queued is not queued twice.
		void rescan();

		// Puts `stepPath` at the head of the queue and starts it next. Does nothing if its mesh is
		// already there — the caller should check meshFor() first and load it directly.
		void prioritise(const QString& stepPath);

	signals:
		// `stepPath`'s mesh is now on disk at `meshPath`. Emitted for background work too, so a
		// viewer showing that model can pick it up without polling.
		void meshReady(const QString& stepPath, const QString& meshPath);
		// Conversion failed; `reason` is the converter's own complaint where it had one.
		void meshFailed(const QString& stepPath, const QString& reason);
		// `done` of `total` conversions finished in the current run. Both are 0 when idle, which
		// is the signal to take the progress message down.
		void progressed(int done, int total);

	private:
		// Starts the next queued conversion, or reports idle when there is nothing left.
		void startNext();
		// Turns the converter's geometry manifest into the cached mesh set: reads the STEP's own
		// colours, pairs them with the solids that came out, and publishes the result under
		// `target`. This is the step that makes a model arrive in its own colours rather than
		// as one lump of grey.
		bool writeMeshSet(const QString& stepPath, const QString& geometryManifest,
			const QString& target);
		// Cleans up after the running process and moves on.
		void finishCurrent(bool ok, const QString& reason);

		DatabaseHandle* m_handle = nullptr;
		QString m_cacheRoot;
		QStringList m_queue;
		QProcess* m_process = nullptr;
		QString m_currentSource;    // the STEP being converted
		QString m_currentTarget;    // its final cache entry
		QString m_currentTemp;      // what the converter actually writes, renamed on success
		QString m_currentScript;    // the generated .py, removed when the process exits
		int m_done = 0;             // finished conversions in the current run
		int m_total = 0;            // queued in the current run, for the progress message
	};

}

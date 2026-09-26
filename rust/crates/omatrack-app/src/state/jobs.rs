//! Named background work the status bar reports.
//!
//! A job lives exactly as long as its [`JobHandle`]: finishing it
//! explicitly ([`Jobs::finish`]) removes it at once, and dropping the handle
//! (a cancelled or replaced task drops its future, and the handle with it)
//! removes it on the next turn of the foreground executor. No path can
//! leave a spinner running for work that no longer exists.

use gpui_kit::{Context, SharedString, Task};

/// Identity of one running job.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct JobId(u64);

/// One running job (presentation snapshot).
#[derive(Debug, Clone, PartialEq)]
pub struct Job {
    id: JobId,
    label: SharedString,
    progress: Option<(usize, usize)>,
}

impl Job {
    pub fn id(&self) -> JobId {
        self.id
    }
    /// What is running, in interface copy (`Scanning library`).
    pub fn label(&self) -> &SharedString {
        &self.label
    }
    /// `(done, total)` when the job can tell.
    pub fn progress(&self) -> Option<(usize, usize)> {
        self.progress
    }
}

/// Ownership of one running job. Move it into the task doing the work;
/// dropping it ends the job.
#[must_use = "dropping the handle ends the job at once"]
#[derive(Debug)]
pub struct JobHandle {
    id: JobId,
    ended: async_channel::Sender<JobId>,
}

impl JobHandle {
    pub fn id(&self) -> JobId {
        self.id
    }
}

impl Drop for JobHandle {
    fn drop(&mut self) {
        // Unbounded: only fails once `Jobs` itself is gone.
        let _ = self.ended.try_send(self.id);
    }
}

/// The list of running jobs. Owners start a job, report progress and
/// finish it (or drop its handle); the status bar observes this entity.
pub struct Jobs {
    next: u64,
    running: Vec<Job>,
    ended: async_channel::Sender<JobId>,
    _reaper: Task<()>,
}

impl Jobs {
    pub fn new(cx: &mut Context<Self>) -> Self {
        let (ended, dropped) = async_channel::unbounded::<JobId>();
        let reaper = cx.spawn(async move |this, cx| {
            while let Ok(id) = dropped.recv().await {
                if this.update(cx, |jobs, cx| jobs.remove(id, cx)).is_err() {
                    break;
                }
            }
        });
        Self {
            next: 0,
            running: Vec::new(),
            ended,
            _reaper: reaper,
        }
    }

    pub fn start(&mut self, label: impl Into<SharedString>, cx: &mut Context<Self>) -> JobHandle {
        self.next += 1;
        let id = JobId(self.next);
        self.running.push(Job {
            id,
            label: label.into(),
            progress: None,
        });
        cx.notify();
        JobHandle {
            id,
            ended: self.ended.clone(),
        }
    }

    pub fn set_progress(&mut self, id: JobId, done: usize, total: usize, cx: &mut Context<Self>) {
        if let Some(job) = self.running.iter_mut().find(|job| job.id == id)
            && job.progress != Some((done, total))
        {
            job.progress = Some((done, total));
            cx.notify();
        }
    }

    /// End the job now (its handle's drop is then a no-op).
    pub fn finish(&mut self, job: JobHandle, cx: &mut Context<Self>) {
        self.remove(job.id, cx);
    }

    fn remove(&mut self, id: JobId, cx: &mut Context<Self>) {
        let before = self.running.len();
        self.running.retain(|job| job.id != id);
        if self.running.len() != before {
            cx.notify();
        }
    }

    pub fn running(&self) -> &[Job] {
        &self.running
    }

    pub fn is_busy(&self) -> bool {
        !self.running.is_empty()
    }
}

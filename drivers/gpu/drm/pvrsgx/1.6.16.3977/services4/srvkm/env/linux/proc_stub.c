#include <linux/kernel.h>
#include <linux/stdarg.h>

#include "services_headers.h"
#include "proc.h"
#include "env_perproc.h"

off_t printAppend(IMG_CHAR *buffer, size_t size, off_t off,
                  const IMG_CHAR *format, ...)
{
        size_t space = size - (size_t)off;
        va_list args;
        IMG_INT length;

        va_start(args, format);
        length = vsnprintf(buffer + off, space, format, args);
        va_end(args);

        if (length >= (IMG_INT)space || length < 0) {
                buffer[size - 1] = 0;
                return (off_t)(size - 1);
        }

        return off + (off_t)length;
}

void *ProcSeq1ElementOff2Element(struct seq_file *sfile, loff_t off)
{
        return off == 0 ? (void *)2 : NULL;
}

void *ProcSeq1ElementHeaderOff2Element(struct seq_file *sfile, loff_t off)
{
        if (off == 0)
                return PVR_PROC_SEQ_START_TOKEN;

        return off == 1 ? (void *)2 : NULL;
}

IMG_INT CreateProcEntries(IMG_VOID)
{
        return 0;
}

IMG_INT CreateProcReadEntry(const IMG_CHAR *name, pvr_read_proc_t handler)
{
        return 0;
}

IMG_INT CreateProcEntry(const IMG_CHAR *name, read_proc_t rhandler,
                        write_proc_t whandler, IMG_VOID *data)
{
        return 0;
}

IMG_INT CreatePerProcessProcEntry(const IMG_CHAR *name, read_proc_t rhandler,
                                  write_proc_t whandler, IMG_VOID *data)
{
        return 0;
}

IMG_VOID RemoveProcEntry(const IMG_CHAR *name)
{
}

IMG_VOID RemovePerProcessProcEntry(const IMG_CHAR *name)
{
}

IMG_VOID RemoveProcEntries(IMG_VOID)
{
}

struct proc_dir_entry *CreateProcReadEntrySeq(
        const IMG_CHAR *name, IMG_VOID *data,
        pvr_next_proc_seq_t next_handler,
        pvr_show_proc_seq_t show_handler,
        pvr_off2element_proc_seq_t off2element_handler,
        pvr_startstop_proc_seq_t startstop_handler)
{
        return NULL;
}

struct proc_dir_entry *CreateProcEntrySeq(
        const IMG_CHAR *name, IMG_VOID *data,
        pvr_next_proc_seq_t next_handler,
        pvr_show_proc_seq_t show_handler,
        pvr_off2element_proc_seq_t off2element_handler,
        pvr_startstop_proc_seq_t startstop_handler,
        write_proc_t whandler)
{
        return NULL;
}

struct proc_dir_entry *CreatePerProcessProcEntrySeq(
        const IMG_CHAR *name, IMG_VOID *data,
        pvr_next_proc_seq_t next_handler,
        pvr_show_proc_seq_t show_handler,
        pvr_off2element_proc_seq_t off2element_handler,
        pvr_startstop_proc_seq_t startstop_handler,
        write_proc_t whandler)
{
        return NULL;
}

IMG_VOID RemoveProcEntrySeq(struct proc_dir_entry *proc_entry)
{
}

IMG_VOID RemovePerProcessProcEntrySeq(struct proc_dir_entry *proc_entry)
{
}

IMG_VOID RemovePerProcessProcDir(PVRSRV_ENV_PER_PROCESS_DATA *psPerProc)
{
}

/* $Id: session.c 13 2011-01-02 16:13:14Z mmatloka $ */
/*
 * PSNC DRMAA for SLURM
 * Copyright (C) 2011 Poznan Supercomputing and Networking Center
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <drmaa_utils/iter.h>
#include <drmaa_utils/conf.h>
#include <slurm_drmaa/job.h>
#include <slurm_drmaa/session.h>
#include <slurm_drmaa/util.h>
#include <slurm_drmaa/slurm_missing.h>

#include <slurm/slurmdb.h>

/* Defines */
#if SLURM_VERSION_NUMBER < SLURM_VERSION_NUM(2,6,0)
#error "SLURM version < 2.6.0 is not supported."
#endif
#define ARRAY_INX_MAXLEN 64

static char *slurmdrmaa_session_run_job(fsd_drmaa_session_t *self, const fsd_template_t *jt);

static fsd_iter_t *slurmdrmaa_session_run_bulk(	fsd_drmaa_session_t *self,const fsd_template_t *jt, int start, int end, int incr );

static fsd_job_t *slurmdrmaa_session_new_job( fsd_drmaa_session_t *self, const char *job_id );

slurmdb_cluster_rec_t *working_cluster_rec = NULL;

fsd_drmaa_session_t *
slurmdrmaa_session_new( const char *contact )
{
	slurmdrmaa_session_t *volatile self = NULL;
	TRY
	 {
		self = (slurmdrmaa_session_t*)fsd_drmaa_session_new(contact);

		fsd_realloc( self, 1, slurmdrmaa_session_t );

		self->super.run_job = slurmdrmaa_session_run_job;
		self->super.run_bulk = slurmdrmaa_session_run_bulk;
		self->super.new_job = slurmdrmaa_session_new_job;

		self->super.load_configuration( &self->super, "slurm_drmaa" );
	 }
	EXCEPT_DEFAULT
	 {
		fsd_free( self );
		fsd_exc_reraise();
	 }
	END_TRY
	return (fsd_drmaa_session_t*)self;
}


char *
slurmdrmaa_session_run_job(
		fsd_drmaa_session_t *self,
		const fsd_template_t *jt
		)
{
	char *job_id = NULL;
	fsd_iter_t *volatile job_ids = NULL;

	TRY
	 {
		job_ids = self->run_bulk( self, jt, 1, 1, 1 ); /* single job run as bulk job specialization */
		job_id = fsd_strdup( job_ids->next( job_ids ) );
	 }
	FINALLY
	 {
		if( job_ids )
			job_ids->destroy( job_ids );
	 }
	END_TRY
	return job_id;
}


fsd_iter_t *
slurmdrmaa_session_run_bulk(
		fsd_drmaa_session_t *self,
		const fsd_template_t *jt,
		int start, int end, int incr )
{
	int ret = 0;
	unsigned i = 0;
	int job_id = 0;
	int task_id = 0;
	fsd_job_t *volatile job = NULL;
	volatile unsigned n_jobs = (end - start) / incr + 1;
	char ** volatile job_ids = fsd_calloc( job_ids, n_jobs + 1, char* );
	volatile bool connection_lock = false;
	fsd_environ_t *volatile env = NULL;
	job_desc_msg_t job_desc;
	submit_response_msg_t *submit_response = NULL;

	slurmdrmaa_init_job_desc( &job_desc );

	TRY
	{
			connection_lock = fsd_mutex_lock( &self->drm_connection_mutex );
			slurmdrmaa_job_create_req( self, jt, (fsd_environ_t**)&env , &job_desc, 0 );

			/* Create job array spec if more than 1 task */
			if(n_jobs > 1)
			{
				fsd_calloc(job_desc.array_inx, ARRAY_INX_MAXLEN, char*);
				ret = snprintf(job_desc.array_inx, ARRAY_INX_MAXLEN, "%d-%d:%d", start, end, incr );
				if (ret < 0 || ret >= ARRAY_INX_MAXLEN) {
					fsd_exc_raise_fmt(FSD_ERRNO_INTERNAL_ERROR, "snprintf: not enough memory");
				}
				fsd_log_debug(("array job '%s' prepared", job_desc.array_inx));
			}

			/* Submit the batch job */
			if(slurm_submit_batch_job(&job_desc, &submit_response) != SLURM_SUCCESS){
				fsd_exc_raise_fmt(
					FSD_ERRNO_INTERNAL_ERROR,"slurm_submit_batch_job: %s",slurm_strerror(slurm_get_errno()));
			}

			connection_lock = fsd_mutex_unlock( &self->drm_connection_mutex );

			/* Watch each job in the array */
			for (i = 0; i < n_jobs; ++i) {
				job_id = (int) submit_response->job_id;
				task_id = start + i*incr;
				if (n_jobs > 1) {
					/* Array job */
					if (!working_cluster_rec)
						job_ids[i] = fsd_asprintf("%d_%d", job_id, task_id); /* .0*/
					else
						job_ids[i] = fsd_asprintf("%d_%d.%s", job_id, task_id, working_cluster_rec->name);
				} else {
					/* Single job */
					if (!working_cluster_rec)
						job_ids[i] = fsd_asprintf("%d", job_id); /* .0*/
					else
						job_ids[i] = fsd_asprintf("%d.%s", job_id, working_cluster_rec->name);
				}

				fsd_log_debug(("job %s submitted", job_ids[i]));
				job = slurmdrmaa_job_new( fsd_strdup(job_ids[i]) );
				job->session = self;
				job->submit_time = time(NULL);
				self->jobs->add( self->jobs, job );
				job->release( job );
				job = NULL;
			}

			if (working_cluster_rec)
				slurmdb_destroy_cluster_rec(working_cluster_rec);

			working_cluster_rec = NULL;
	 }
	 ELSE
	{
		if ( !connection_lock )
			connection_lock = fsd_mutex_lock( &self->drm_connection_mutex );

		if ( submit_response )
			slurm_free_submit_response_response_msg ( submit_response );
	}
	FINALLY
	 {
		if( connection_lock )
			fsd_mutex_unlock( &self->drm_connection_mutex );

		if( job )
			job->release( job );

		if( fsd_exc_get() != NULL ) {
			fsd_free_vector( job_ids );
			n_jobs = 0;
		}

		slurmdrmaa_free_job_desc(&job_desc);
	 }
	END_TRY


	return fsd_iter_new( job_ids, n_jobs );
}


fsd_job_t *
slurmdrmaa_session_new_job( fsd_drmaa_session_t *self, const char *job_id )
{
	fsd_job_t *job;
	job = slurmdrmaa_job_new( fsd_strdup(job_id) );
	job->session = self;
	return job;
}


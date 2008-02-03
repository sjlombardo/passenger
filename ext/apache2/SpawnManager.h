#ifndef _PASSENGER_SPAWN_MANAGER_H_
#define _PASSENGER_SPAWN_MANAGER_H_

#include <string>
#include <list>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdarg>
#include <unistd.h>
#include <errno.h>

#include "Application.h"
#include "MessageChannel.h"
#include "Exceptions.h"
#include "Utils.h"

namespace Passenger {

using namespace std;
using namespace boost;

/**
 * This class is responsible for spawning new instances of Ruby on Rails applications.
 * Use the spawn() method to do so.
 *
 * This class is fully thread-safe.
 *
 * <h2>Implementation details</h2>
 * Internally, it makes use of a spawn server, which is written in Ruby. This server
 * is automatically started when a SpawnManager instance is created, and automatically
 * shutdown when that instance is destroyed. Spawning requests are sent to the server,
 * and details about the spawned process is returned.
 *
 * If the spawn server dies during the middle of an operation, it will be restarted.
 * See spawn() for full details.
 *
 * The communication channel with the server is anonymous, i.e. no other processes
 * can access the communication channel, so communication is guaranteed to be safe
 * (unless, of course, if the spawn server itself is a trojan).
 *
 * The server will try to keep the spawning time as small as possible, by keeping
 * corresponding Ruby on Rails frameworks and application code in memory. So the second
 * time an instance of the same application is spawned, the spawn time is significantly
 * lower than the first time. Nevertheless, spawning is a relatively expensive operation
 * (compared to the processing of a typical HTTP request/response), and so should be
 * avoided whenever possible.
 *
 * See the documentation of the spawn server for full implementation details.
 */
class SpawnManager {
private:
	string spawnServerCommand;
	string logFile;
	string environment;
	string rubyCommand;
	
	mutex lock;
	
	MessageChannel channel;
	pid_t pid;
	bool serverNeedsRestart;

	/**
	 * Restarts the spawn server.
	 *
	 * @throws SystemException An error occured while trying to setup the spawn server.
	 * @throws IOException The specified log file could not be opened.
	 */
	void restartServer() {
		if (pid != 0) {
			channel.close();
			// TODO: should not wait infinitely
			waitpid(pid, NULL, 0);
			pid = 0;
		}
		
		int fds[2];
		FILE *logFileHandle = NULL;
		
		serverNeedsRestart = true;
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1) {
			throw SystemException("Cannot create a Unix socket", errno);
		}
		if (!logFile.empty()) {
			logFileHandle = fopen(logFile.c_str(), "a");
			if (logFileHandle == NULL) {
				string message("Cannot open log file '");
				message.append(logFile);
				message.append("' for writing.");
				throw IOException(message);
			}
		}

		pid = fork();
		if (pid == 0) {
			if (!logFile.empty()) {
				dup2(fileno(logFileHandle), STDERR_FILENO);
				fclose(logFileHandle);
			}
			dup2(STDERR_FILENO, STDOUT_FILENO);
			if (!environment.empty()) {
				setenv("RAILS_ENV", environment.c_str(), true);
			}
			dup2(fds[1], STDIN_FILENO);
			close(fds[0]);
			close(fds[1]);
			
			// Close all other file descriptors
			for (long i = sysconf(_SC_OPEN_MAX) - 1; i >= 0; i--) {
				if (i != STDIN_FILENO && i != STDOUT_FILENO && i != STDERR_FILENO) {
					close(i);
				}
			}
			
			execlp(rubyCommand.c_str(), rubyCommand.c_str(), spawnServerCommand.c_str(), NULL);
			int e = errno;
			fprintf(stderr, "Unable to run %s: %s\n", rubyCommand.c_str(), strerror(e));
			fflush(stderr);
			_exit(1);
		} else if (pid == -1) {
			int e = errno;
			close(fds[0]);
			close(fds[1]);
			if (logFileHandle != NULL) {
				fclose(logFileHandle);
			}
			pid = 0;
			throw SystemException("Unable to fork a process", e);
		} else {
			close(fds[1]);
			if (!logFile.empty()) {
				fclose(logFileHandle);
			}
			channel = MessageChannel(fds[0]);
			serverNeedsRestart = false;
		}
	}

public:
	/**
	 * Thrown when SpawnManager tried to restart the spawn server, but failed.
	 * Use getSubException() to find out more details about the failure.
	 */
	class RestartException: public exception {
	private:
		shared_ptr<exception> m_subexception;
	public:
		RestartException(shared_ptr<exception> subexception)
			: m_subexception(subexception) {}
		
		virtual ~RestartException() throw() {}
		
		virtual const char *what() const throw() {
			return m_subexception->what();
		}
	
		/**
		 * An exception which contains more details about why the restart failed.
		 */
		shared_ptr<exception> getSubException() const throw() {
			return m_subexception;
		}
	};

	/**
	 * Construct a new SpawnManager.
	 *
	 * @param spawnServerCommand The filename of the spawn server to use.
	 * @param logFile Specify a log file that the spawn manager should use.
	 *            Messages on its standard output and standard error channels
	 *            will be written to this log file. If an empty string is
	 *            specified, no log file will be used, and the spawn server
	 *            will use the same standard output/error channels as the
	 *            current process.
	 * @param environment The RAILS_ENV environment that all RoR applications
	 *            should use. If an empty string is specified, the current value
	 *            of the RAILS_ENV environment variable will be used.
	 * @param rubyCommand The Ruby interpreter's command.
	 * @throws SystemException An error occured while trying to setup the spawn server.
	 * @throws IOException The specified log file could not be opened.
	 */
	SpawnManager(const string &spawnServerCommand,
	             const string &logFile = "",
	             const string &environment = "production",
	             const string &rubyCommand = "ruby") {
		this->spawnServerCommand = spawnServerCommand;
		this->logFile = logFile;
		this->environment = environment;
		this->rubyCommand = rubyCommand;
		pid = 0;
		restartServer();
	}
	
	~SpawnManager() throw() {
		if (pid != 0) {
			channel.close();
			waitpid(pid, NULL, 0);
		}
	}
	
	/**
	 * Spawn a new instance of a Ruby on Rails application.
	 *
	 * If something went wrong in the server during the spawning process,
	 * then an IOException or a SystemException will be thrown. The server will
	 * be restarted next time spawn() is called. If the server crashes during
	 * the restart, a RestartException will be thrown.
	 *
	 * @param appRoot The application root of a RoR application, i.e. the folder that
	 *             contains 'app/', 'public/', 'config/', etc. This must be a valid directory,
	 *             but the path does not have to be absolute.
	 * @param user The user
	 * @return A smart pointer to an Application object, which represents the application
	 *         instance that has been spawned. Use this object to communicate with the
	 *         spawned application.
	 * @throws IOException Something went wrong during spawning.
	 * @throws SystemException Something went wrong during spawning.
	 * @throws RestartException An attempt to restart the spawn server was made, but that failed.
	 */
	ApplicationPtr spawn(const string &appRoot, const string &user = "", const string &group = "") {
		vector<string> args;
		mutex::scoped_lock l(lock);
		
		if (serverNeedsRestart) {
			// TODO: This is not the best place to restart the server.
			// Ideally a spawn() should fail as least as possible.
			try {
				P_TRACE("Restarting spawn server.");
				restartServer();
			} catch (const IOException &e) {
				P_TRACE("Failed to restart spawn server: " << e.what());
				shared_ptr<IOException> copy(new IOException(e));
				throw RestartException(copy);
			} catch (const SystemException &e) {
				P_TRACE("Failed to restart spawn server: " << e.what());
				shared_ptr<SystemException> copy(new SystemException(e));
				throw RestartException(copy);
			}
		}
	
		try {
			channel.write("spawn_application", appRoot.c_str(), user.c_str(), group.c_str(), NULL);
			if (!channel.read(args)) {
				throw IOException("The spawn server has exited unexpectedly.");
			}
			pid_t pid = atoi(args.front().c_str());
			int listenSocket = channel.readFileDescriptor();
			return ApplicationPtr(new Application(appRoot, pid, listenSocket));
		} catch (const exception &e) {
			P_TRACE("Spawn server died. Will restart it next time.");
			serverNeedsRestart = true;
			throw;
		}
	}
};

typedef shared_ptr<SpawnManager> SpawnManagerPtr;

} // namespace Passenger

#endif /* _PASSENGER_SPAWN_MANAGER_H_ */
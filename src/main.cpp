/* Copyright 2013-2017 Sathya Laufer
 *
 * Homegear is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Homegear is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Homegear.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU Lesser General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
*/

#include "GD/GD.h"
#include "Monitor.h"
#include "CLI/CLIClient.h"
#include "ScriptEngine/ScriptEngineClient.h"
#include "Flows/FlowsClient.h"
#include "UPnP/UPnP.h"
#include "MQTT/Mqtt.h"
#include <homegear-base/BaseLib.h>
#include "../config.h"

#include <readline/readline.h>
#include <readline/history.h>
#include <execinfo.h>
#include <signal.h>
#include <wait.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/prctl.h> //For function prctl
#include <sys/sysctl.h> //For BSD systems
#include <malloc.h>

#include <cmath>
#include <vector>
#include <memory>
#include <algorithm>

#include <gcrypt.h>

void startMainProcess();
void startUp();

GCRY_THREAD_OPTION_PTHREAD_IMPL;

Monitor _monitor;
std::mutex _shuttingDownMutex;
std::atomic_bool _reloading;
bool _fork = false;
std::atomic_bool _monitorProcess;
pid_t _mainProcessId = 0;
bool _startAsDaemon = false;
bool _nonInteractive = false;
std::atomic_bool _startUpComplete;
std::atomic_bool _shutdownQueued;
bool _disposing = false;
std::shared_ptr<std::function<void(int32_t, std::string)>> _errorCallback;

void exitHomegear(int exitCode)
{
	if(GD::familyController) GD::familyController->disposeDeviceFamilies();
	if(GD::bl->db)
	{
		//Finish database operations before closing modules, otherwise SEGFAULT
		GD::bl->db->dispose();
		GD::bl->db.reset();
	}
    if(GD::familyController) GD::familyController->dispose();
    if(GD::licensingController) GD::licensingController->dispose();
    _monitor.stop();
    exit(exitCode);
}

void bindRPCServers()
{
	BaseLib::TcpSocket tcpSocket(GD::bl.get());
	// Bind all RPC servers listening on ports <= 1024
	for(int32_t i = 0; i < GD::serverInfo.count(); i++)
	{
		BaseLib::Rpc::PServerInfo settings = GD::serverInfo.get(i);
		if(settings->port > 1024) continue;
		std::string info = "Info: Binding XML RPC server " + settings->name + " listening on " + settings->interface + ":" + std::to_string(settings->port);
		if(settings->ssl) info += ", SSL enabled";
		else GD::bl->rpcPort = settings->port;
		if(settings->authType != BaseLib::Rpc::ServerInfo::Info::AuthType::none) info += ", authentication enabled";
		info += "...";
		GD::out.printInfo(info);
		settings->socketDescriptor = tcpSocket.bindSocket(settings->interface, std::to_string(settings->port), settings->address);
		if(settings->socketDescriptor) GD::out.printInfo("Info: Server successfully bound.");
	}
}

void startRPCServers()
{
	for(int32_t i = 0; i < GD::serverInfo.count(); i++)
	{
		BaseLib::Rpc::PServerInfo settings = GD::serverInfo.get(i);
		std::string info = "Starting XML RPC server " + settings->name + " listening on " + settings->interface + ":" + std::to_string(settings->port);
		if(settings->ssl) info += ", SSL enabled";
		else GD::bl->rpcPort = settings->port;
		if(settings->authType != BaseLib::Rpc::ServerInfo::Info::AuthType::none) info += ", authentication enabled";
		info += "...";
		GD::out.printInfo(info);
		GD::rpcServers[i].start(settings);
	}
	if(GD::rpcServers.size() == 0)
	{
		GD::out.printCritical("Critical: No RPC servers are running. Terminating Homegear.");
		exitHomegear(1);
	}

}

void stopRPCServers(bool dispose)
{
	GD::out.printInfo( "(Shutdown) => Stopping RPC servers");
	for(std::map<int32_t, Rpc::Server>::iterator i = GD::rpcServers.begin(); i != GD::rpcServers.end(); ++i)
	{
		i->second.stop();
		if(dispose) i->second.dispose();
	}
	GD::bl->rpcPort = 0;
	//Don't clear map!!! Server is still accessed i. e. by the event handler!
}

void sigchld_handler(int32_t signalNumber)
{
	try
	{
		pid_t pid;
		int status;

		while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		{
			int32_t exitStatus = WEXITSTATUS(status);
			int32_t signal = -1;
			bool coreDumped = false;
			if(WIFSIGNALED(status))
			{
				signal = WTERMSIG(status);
				if(WCOREDUMP(status)) coreDumped = true;
			}

			GD::out.printInfo("Info: Process with id " + std::to_string(pid) + " ended.");
			if(pid == _mainProcessId)
			{
				_mainProcessId = 0;
				bool stop = false;
				if(signal != -1)
				{
					if(signal == SIGTERM || signal == SIGINT || signal == SIGQUIT || (signal == SIGKILL && !_monitor.killedProcess())) stop = true;
					if(signal == SIGKILL && !_monitor.killedProcess()) GD::out.printWarning("Warning: SIGKILL (signal 9) used to stop Homegear. Please shutdown Homegear properly to avoid database corruption.");
					if(coreDumped) GD::out.printError("Error: Core was dumped.");
				}
				else stop = true;
				if(stop)
				{
					GD::out.printInfo("Info: Homegear exited with exit code " + std::to_string(exitStatus) + ". Stopping monitor process.");
					exit(0);
				}

				GD::out.printError("Homegear was terminated. Restarting (1)...");
				_monitor.suspend();
				_fork = true;
			}
			else
			{
#ifndef NO_SCRIPTENGINE
				if(GD::scriptEngineServer) GD::scriptEngineServer->processKilled(pid, exitStatus, signal, coreDumped);
#endif
				if(GD::flowsServer) GD::flowsServer->processKilled(pid, exitStatus, signal, coreDumped);
			}
		}
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void terminate(int signalNumber)
{
	try
	{
		if (signalNumber == SIGTERM || signalNumber == SIGINT)
		{
			if(_monitorProcess)
			{
				GD::out.printMessage("Info: Redirecting signal to child process...");
				if(_mainProcessId != 0) kill(_mainProcessId, SIGTERM);
				else _exit(0);
				return;
			}

			_shuttingDownMutex.lock();
			if(!_startUpComplete)
			{
				GD::out.printMessage("Info: Startup is not complete yet. Queueing shutdown.");
				_shutdownQueued = true;
				_shuttingDownMutex.unlock();
				return;
			}
			if(GD::bl->shuttingDown)
			{
				_shuttingDownMutex.unlock();
				return;
			}
			GD::out.printMessage("(Shutdown) => Stopping Homegear (Signal: " + std::to_string(signalNumber) + ")");
			GD::bl->shuttingDown = true;
			_shuttingDownMutex.unlock();
			if(GD::ipcServer) GD::ipcServer->homegearShuttingDown();
			if(GD::flowsServer) GD::flowsServer->homegearShuttingDown(); //Needs to be called before familyController->homegearShuttingDown()
#ifndef NO_SCRIPTENGINE
			if(GD::scriptEngineServer) GD::scriptEngineServer->homegearShuttingDown(); //Needs to be called before familyController->homegearShuttingDown()
#endif
			if(GD::familyController) GD::familyController->homegearShuttingDown();
			_disposing = true;
			GD::out.printInfo("(Shutdown) => Stopping CLI server");
			if(GD::cliServer) GD::cliServer->stop();
			if(GD::bl->settings.enableUPnP())
			{
				GD::out.printInfo("Stopping UPnP server...");
				GD::uPnP->stop();
			}
#ifdef EVENTHANDLER
			GD::out.printInfo( "(Shutdown) => Stopping Event handler");
			if(GD::eventHandler) GD::eventHandler->dispose();
#endif
			stopRPCServers(true);
			GD::rpcServers.clear();

			if(GD::mqtt && GD::mqtt->enabled())
			{
				GD::out.printInfo( "(Shutdown) => Stopping MQTT client");;
				GD::mqtt->stop();
			}
			GD::out.printInfo( "(Shutdown) => Stopping RPC client");;
			if(GD::rpcClient) GD::rpcClient->dispose();
			GD::out.printInfo( "(Shutdown) => Closing physical interfaces");
			if(GD::familyController) GD::familyController->physicalInterfaceStopListening();
			GD::out.printInfo("(Shutdown) => Stopping IPC server...");
			if(GD::ipcServer) GD::ipcServer->stop();
			if(GD::bl->settings.enableFlows()) GD::out.printInfo("(Shutdown) => Stopping flows server...");
			if(GD::flowsServer) GD::flowsServer->stop();
#ifndef NO_SCRIPTENGINE
			GD::out.printInfo("(Shutdown) => Stopping script engine server...");
			if(GD::scriptEngineServer) GD::scriptEngineServer->stop();
#endif
			GD::out.printMessage("(Shutdown) => Saving device families");
			if(GD::familyController) GD::familyController->save(false);
			GD::out.printMessage("(Shutdown) => Disposing device families");
			if(GD::familyController) GD::familyController->disposeDeviceFamilies();
			GD::out.printMessage("(Shutdown) => Disposing database");
			if(GD::bl->db)
			{
				//Finish database operations before closing modules, otherwise SEGFAULT
				GD::bl->db->dispose();
				GD::bl->db.reset();
			}
			GD::out.printMessage("(Shutdown) => Disposing family modules");
			GD::familyController->dispose();
			GD::out.printMessage("(Shutdown) => Disposing licensing modules");
			if(GD::licensingController) GD::licensingController->dispose();
			GD::bl->fileDescriptorManager.dispose();
			_monitor.stop();
			GD::out.printMessage("(Shutdown) => Shutdown complete.");
			if(_startAsDaemon || _nonInteractive)
			{
				fclose(stdout);
				fclose(stderr);
			}
			gnutls_global_deinit();
			gcry_control(GCRYCTL_SUSPEND_SECMEM_WARN);
			gcry_control(GCRYCTL_TERM_SECMEM);
			gcry_control(GCRYCTL_RESUME_SECMEM_WARN);
			_exit(0);
		}
		else if(signalNumber == SIGHUP)
		{
			if(_monitorProcess)
			{
				GD::out.printMessage("Info: Redirecting signal to child process...");
				if(_mainProcessId != 0) kill(_mainProcessId, SIGHUP);
				return;
			}

			GD::out.printMessage("Info: SIGHUP received...");
			_shuttingDownMutex.lock();
			GD::out.printMessage("Info: Reloading...");
			if(!_startUpComplete)
			{
				_shuttingDownMutex.unlock();
				GD::out.printError("Error: Cannot reload. Startup is not completed.");
				return;
			}
			_startUpComplete = false;
			_shuttingDownMutex.unlock();
			/*if(GD::bl->settings.changed())
			{
				if(GD::bl->settings.enableUPnP())
				{
					GD::out.printInfo("Stopping UPnP server");
					GD::uPnP->stop();
				}
				stopRPCServers(false);
				if(GD::mqtt->enabled())
				{
					GD::out.printInfo( "(Shutdown) => Stopping MQTT client");;
					GD::mqtt->stop();
				}
				if(GD::familyController) GD::familyController->physicalInterfaceStopListening();
				//Binding fails sometimes with "address is already in use" without waiting.
				std::this_thread::sleep_for(std::chrono::milliseconds(10000));
				GD::out.printMessage("Reloading settings...");
				GD::bl->settings.load(GD::configPath + "main.conf");
				GD::clientSettings.load(GD::bl->settings.clientSettingsPath());
				GD::serverInfo.load(GD::bl->settings.serverSettingsPath());
				GD::mqtt->loadSettings();
				if(GD::mqtt->enabled())
				{
					GD::out.printInfo("Starting MQTT client");;
					GD::mqtt->start();
				}
				if(GD::familyController) GD::familyController->physicalInterfaceStartListening();
				startRPCServers();
				if(GD::bl->settings.enableUPnP())
				{
					GD::out.printInfo("Starting UPnP server");
					GD::uPnP->start();
				}
			}*/
			//Reopen log files, important for logrotate
			if(_startAsDaemon || _nonInteractive)
			{
				if(!std::freopen((GD::bl->settings.logfilePath() + "homegear.log").c_str(), "a", stdout))
				{
					GD::out.printError("Error: Could not redirect output to new log file.");
				}
				if(!std::freopen((GD::bl->settings.logfilePath() + "homegear.err").c_str(), "a", stderr))
				{
					GD::out.printError("Error: Could not redirect errors to new log file.");
				}
			}
			GD::out.printCritical("Info: Backing up database...");
			GD::bl->db->hotBackup();
			if(!GD::bl->db->isOpen())
			{
				GD::out.printCritical("Critical: Can't reopen database. Exiting...");
				_exit(1);
			}
			GD::out.printInfo("Reloading flows server...");
			if(GD::flowsServer) GD::flowsServer->homegearReloading();
#ifndef NO_SCRIPTENGINE
			GD::out.printInfo("Reloading script engine server...");
			if(GD::scriptEngineServer) GD::scriptEngineServer->homegearReloading();
#endif
			_shuttingDownMutex.lock();
			_startUpComplete = true;
			if(_shutdownQueued)
			{
				_shuttingDownMutex.unlock();
				terminate(SIGTERM);
			}
			_shuttingDownMutex.unlock();
			GD::out.printInfo("Info: Reload complete.");
		}
		else
		{
			if (!_disposing) GD::out.printCritical("Signal " + std::to_string(signalNumber) + " received.");
			signal(signalNumber, SIG_DFL); //Reset signal handler for the current signal to default
			kill(getpid(), signalNumber); //Generate core dump
		}
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void errorCallback(int32_t level, std::string message)
{
	if(GD::rpcClient) GD::rpcClient->broadcastError(level, message);
}

int32_t getIntInput()
{
	std::string input;
	std::cin >> input;
	int32_t intInput = -1;
	try	{ intInput = std::stoll(input); } catch(...) {}
    return intInput;
}

int32_t getHexInput()
{
	std::string input;
	std::cin >> input;
	int32_t intInput = -1;
	try	{ intInput = std::stoll(input, 0, 16); } catch(...) {}
    return intInput;
}

void getExecutablePath(int argc, char* argv[])
{
	char path[1024];
	if(!getcwd(path, sizeof(path)))
	{
		std::cerr << "Could not get working directory." << std::endl;
		exit(1);
	}
	GD::workingDirectory = std::string(path);
#ifdef KERN_PROC //BSD system
	int mib[4];
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PATHNAME;
	mib[3] = -1;
	size_t cb = sizeof(path);
	int result = sysctl(mib, 4, path, &cb, NULL, 0);
	if(result == -1)
	{
		std::cerr << "Could not get executable path." << std::endl;
		exit(1);
	}
	path[sizeof(path) - 1] = '\0';
	GD::executablePath = std::string(path);
	GD::executablePath = GD::executablePath.substr(0, GD::executablePath.find_last_of("/") + 1);
#else
	int length = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (length < 0)
	{
		std::cerr << "Could not get executable path." << std::endl;
		exit(1);
	}
	if((unsigned)length > sizeof(path))
	{
		std::cerr << "The path the homegear binary is in has more than 1024 characters." << std::endl;
		exit(1);
	}
	path[length] = '\0';
	GD::executablePath = std::string(path);
	GD::executablePath = GD::executablePath.substr(0, GD::executablePath.find_last_of("/") + 1);
#endif

	GD::executableFile = std::string(argc > 0 ? argv[0] : "homegear");
	BaseLib::HelperFunctions::trim(GD::executableFile);
	if(GD::executableFile.empty()) GD::executableFile = "homegear";
	std::pair<std::string, std::string> pathNamePair = BaseLib::HelperFunctions::splitLast(GD::executableFile, '/');
	if(!pathNamePair.second.empty()) GD::executableFile = pathNamePair.second;
}

void initGnuTls()
{
	// {{{ Init gcrypt and GnuTLS
		gcry_error_t gcryResult;
		if((gcryResult = gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread)) != GPG_ERR_NO_ERROR)
		{
			GD::out.printCritical("Critical: Could not enable thread support for gcrypt.");
			exit(2);
		}

		if (!gcry_check_version(GCRYPT_VERSION))
		{
			GD::out.printCritical("Critical: Wrong gcrypt version.");
			exit(2);
		}
		gcry_control(GCRYCTL_SUSPEND_SECMEM_WARN);
		if((gcryResult = gcry_control(GCRYCTL_INIT_SECMEM, (int)GD::bl->settings.secureMemorySize(), 0)) != GPG_ERR_NO_ERROR)
		{
			GD::out.printCritical("Critical: Could not allocate secure memory. Error code is: " + std::to_string((int32_t)gcryResult));
			exit(2);
		}
		gcry_control(GCRYCTL_RESUME_SECMEM_WARN);
		gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

		int32_t gnutlsResult = 0;
		if((gnutlsResult = gnutls_global_init()) != GNUTLS_E_SUCCESS)
		{
			GD::out.printCritical("Critical: Could not initialize GnuTLS: " + std::string(gnutls_strerror(gnutlsResult)));
			exit(2);
		}
	// }}}
}

void setLimits()
{
	struct rlimit limits;
	if(!GD::bl->settings.enableCoreDumps()) prctl(PR_SET_DUMPABLE, 0);
	else
	{
		//Set rlimit for core dumps
		getrlimit(RLIMIT_CORE, &limits);
		limits.rlim_cur = limits.rlim_max;
		GD::out.printInfo("Info: Setting allowed core file size to \"" + std::to_string(limits.rlim_cur) + "\" for user with id " + std::to_string(getuid()) + " and group with id " + std::to_string(getgid()) + '.');
		setrlimit(RLIMIT_CORE, &limits);
		getrlimit(RLIMIT_CORE, &limits);
		GD::out.printInfo("Info: Core file size now is \"" + std::to_string(limits.rlim_cur) + "\".");
	}
#ifdef RLIMIT_RTPRIO //Not existant on BSD systems
	getrlimit(RLIMIT_RTPRIO, &limits);
	limits.rlim_cur = limits.rlim_max;
	GD::out.printInfo("Info: Setting maximum thread priority to \"" + std::to_string(limits.rlim_cur) + "\" for user with id " + std::to_string(getuid()) + " and group with id " + std::to_string(getgid()) + '.');
	setrlimit(RLIMIT_RTPRIO, &limits);
	getrlimit(RLIMIT_RTPRIO, &limits);
	GD::out.printInfo("Info: Maximum thread priority now is \"" + std::to_string(limits.rlim_cur) + "\".");
#endif
}

void printHelp()
{
	std::cout << "Usage: homegear [OPTIONS]" << std::endl << std::endl;
	std::cout << "Option              Meaning" << std::endl;
	std::cout << "-h                  Show this help" << std::endl;
	std::cout << "-u                  Run as user" << std::endl;
	std::cout << "-g                  Run as group" << std::endl;
	std::cout << "-c <path>           Specify path to config file" << std::endl;
	std::cout << "-d                  Run as daemon" << std::endl;
	std::cout << "-p <pid path>       Specify path to process id file" << std::endl;
	std::cout << "-s <user> <group>   Set GPIO settings and necessary permissions for all defined physical devices" << std::endl;
	std::cout << "-r                  Connect to Homegear on this machine" << std::endl;
	std::cout << "-e <command>        Execute CLI command" << std::endl;
	std::cout << "-o <input> <output> Convert old device description file into new format." << std::endl;
	std::cout << "-l                  Checks the lifeticks of all components. Exit code \"0\" means everything is ok." << std::endl;
	std::cout << "-v                  Print program version" << std::endl;
}

void startMainProcess()
{
	try
	{
		_monitor.stop();
		_fork = false;
		_monitorProcess = false;
		_monitor.init();

		pid_t pid, sid;
		pid = fork();
		if(pid < 0)
		{
			exitHomegear(1);
		}
		else if(pid > 0)
		{
			_monitorProcess = true;
			_mainProcessId = pid;
			_monitor.prepareParent();
		}
		else
		{
			//Set process permission
			umask(S_IWGRP | S_IWOTH);

			//Set child processe's id
			sid = setsid();
			if(sid < 0)
			{
				exitHomegear(1);
			}

			_monitor.prepareChild();
		}
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void startDaemon()
{
	try
	{
		pid_t pid, sid;
		pid = fork();
		if(pid < 0)
		{
			exitHomegear(1);
		}
		if(pid > 0)
		{
			exitHomegear(0);
		}

		//Set process permission
		umask(S_IWGRP | S_IWOTH);

		//Set child processe's id
		sid = setsid();
		if(sid < 0)
		{
			exitHomegear(1);
		}

		close(STDIN_FILENO);

		startMainProcess();
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void startUp()
{
	try
	{
		if((chdir(GD::bl->settings.workingDirectory().c_str())) < 0)
		{
			GD::out.printError("Could not change working directory to " + GD::bl->settings.workingDirectory() + ".");
			exitHomegear(1);
		}

    	struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = terminate;

		//Use sigaction over signal because of different behavior in Linux and BSD
    	sigaction(SIGHUP, &sa, NULL);
    	sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGINT, &sa, NULL);
    	sigaction(SIGABRT, &sa, NULL);
    	sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGQUIT, &sa, NULL);
		sigaction(SIGILL, &sa, NULL);
		sigaction(SIGABRT, &sa, NULL);
		sigaction(SIGFPE, &sa, NULL);
		sigaction(SIGALRM, &sa, NULL);
		sigaction(SIGUSR1, &sa, NULL);
		sigaction(SIGUSR2, &sa, NULL);
		sigaction(SIGTSTP, &sa, NULL);
		sigaction(SIGTTIN, &sa, NULL);
		sigaction(SIGTTOU, &sa, NULL);
    	sa.sa_handler = sigchld_handler;
    	sigaction(SIGCHLD, &sa, NULL);
		sa.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &sa, NULL);

    	if(_startAsDaemon || _nonInteractive)
		{
			if(!std::freopen((GD::bl->settings.logfilePath() + "homegear.log").c_str(), "a", stdout))
			{
				GD::out.printError("Error: Could not redirect output to log file.");
			}
			if(!std::freopen((GD::bl->settings.logfilePath() + "homegear.err").c_str(), "a", stderr))
			{
				GD::out.printError("Error: Could not redirect errors to log file.");
			}
		}

    	GD::out.printMessage("Starting Homegear...");
    	GD::out.printMessage(std::string("Homegear version ") + VERSION);
    	GD::out.printMessage(std::string("Git commit SHA of libhomegear-base: ") + GITCOMMITSHABASE);
    	GD::out.printMessage(std::string("Git branch of libhomegear-base:     ") + GITBRANCHBASE);
    	GD::out.printMessage(std::string("Git commit SHA of Homegear:         ") + GITCOMMITSHAHOMEGEAR);
    	GD::out.printMessage(std::string("Git branch of Homegear:             ") + GITBRANCHHOMEGEAR);

    	if(GD::bl->settings.memoryDebugging()) mallopt(M_CHECK_ACTION, 3); //Print detailed error message, stack trace, and memory, and abort the program. See: http://man7.org/linux/man-pages/man3/mallopt.3.html
    	if(_monitorProcess)
    	{
    		setLimits();

    		while(_monitorProcess)
    		{
    			std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    			if(_fork)
    			{
    				GD::out.printError("Homegear was terminated. Restarting (2)...");
    				startMainProcess();
    			}
    			_monitor.checkHealth(_mainProcessId);
    		}
    	}

    	initGnuTls();

		if(!GD::bl->io.directoryExists(GD::bl->settings.socketPath()))
		{
			if(!GD::bl->io.createDirectory(GD::bl->settings.socketPath(), S_IRWXU | S_IRWXG))
			{
				GD::out.printCritical("Critical: Directory \"" + GD::bl->settings.socketPath() + "\" does not exist and cannot be created.");
				exit(1);
			}
			if(GD::bl->userId != 0 || GD::bl->groupId != 0)
			{
				if(chown(GD::bl->settings.socketPath().c_str(), GD::bl->userId, GD::bl->groupId) == -1)
				{
					GD::out.printCritical("Critical: Could not set permissions on directory \"" + GD::bl->settings.socketPath() + "\"");
					exit(1);
				}
			}
		}

		setLimits();

		GD::bl->db->init();
		std::string databasePath = GD::bl->settings.databasePath();
		if(databasePath.empty()) databasePath = GD::bl->settings.dataPath();
		std::string databaseBackupPath = GD::bl->settings.databaseBackupPath();
		if(databaseBackupPath.empty()) databaseBackupPath = GD::bl->settings.dataPath();
    	GD::bl->db->open(databasePath, "db.sql", GD::bl->settings.databaseSynchronous(), GD::bl->settings.databaseMemoryJournal(), GD::bl->settings.databaseWALJournal(), databaseBackupPath, "db.sql.bak");
    	if(!GD::bl->db->isOpen()) exitHomegear(1);

        GD::out.printInfo("Initializing database...");
        if(GD::bl->db->convertDatabase()) exitHomegear(0);
        GD::bl->db->initializeDatabase();

        {
        	bool runningAsUser = !GD::runAsUser.empty() && !GD::runAsGroup.empty();

			std::string currentPath = GD::bl->settings.dataPath();
			if(!currentPath.empty() && runningAsUser)
			{
				uid_t userId = GD::bl->hf.userId(GD::bl->settings.dataPathUser());
				gid_t groupId = GD::bl->hf.groupId(GD::bl->settings.dataPathGroup());
				if(((int32_t)userId) == -1 || ((int32_t)groupId) == -1)
				{
					userId = GD::bl->userId;
					groupId = GD::bl->groupId;
				}
				std::vector<std::string> files;
				try
				{
					files = GD::bl->io.getFiles(currentPath, false);
				}
				catch(const std::exception& ex)
				{
					GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
				}
				catch(BaseLib::Exception& ex)
				{
					GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
				}
				catch(...)
				{
					GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
				}
				for(std::vector<std::string>::iterator k = files.begin(); k != files.end(); ++k)
				{
					if((*k).compare(0, 6, "db.sql") != 0) continue;
					std::string file = currentPath + *k;
					if(chown(file.c_str(), userId, groupId) == -1) GD::out.printError("Could not set owner on " + file);
					if(chmod(file.c_str(), GD::bl->settings.dataPathPermissions()) == -1) GD::out.printError("Could not set permissions on " + file);
				}
			}

			currentPath = GD::bl->settings.databasePath();
			if(!currentPath.empty() && runningAsUser)
			{
				uid_t userId = GD::bl->hf.userId(GD::bl->settings.dataPathUser());
				gid_t groupId = GD::bl->hf.groupId(GD::bl->settings.dataPathGroup());
				if(((int32_t)userId) == -1 || ((int32_t)groupId) == -1)
				{
					userId = GD::bl->userId;
					groupId = GD::bl->groupId;
				}
				std::vector<std::string> files;
				try
				{
					files = GD::bl->io.getFiles(currentPath, false);
				}
				catch(const std::exception& ex)
				{
					GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
				}
				catch(BaseLib::Exception& ex)
				{
					GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
				}
				catch(...)
				{
					GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
				}
				for(std::vector<std::string>::iterator k = files.begin(); k != files.end(); ++k)
				{
					std::string file = currentPath + *k;
					if(chown(file.c_str(), userId, groupId) == -1) GD::out.printError("Could not set owner on " + file);
					if(chmod(file.c_str(), GD::bl->settings.dataPathPermissions()) == -1) GD::out.printError("Could not set permissions on " + file);
				}
			}

			currentPath = GD::bl->settings.databaseBackupPath();
			if(!currentPath.empty() && runningAsUser)
			{
				uid_t userId = GD::bl->hf.userId(GD::bl->settings.dataPathUser());
				gid_t groupId = GD::bl->hf.groupId(GD::bl->settings.dataPathGroup());
				if(((int32_t)userId) == -1 || ((int32_t)groupId) == -1)
				{
					userId = GD::bl->userId;
					groupId = GD::bl->groupId;
				}
				std::vector<std::string> files;
				try
				{
					files = GD::bl->io.getFiles(currentPath, false);
				}
				catch(const std::exception& ex)
				{
					GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
				}
				catch(BaseLib::Exception& ex)
				{
					GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
				}
				catch(...)
				{
					GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
				}
				for(std::vector<std::string>::iterator k = files.begin(); k != files.end(); ++k)
				{
					std::string file = currentPath + *k;
					if(chown(file.c_str(), userId, groupId) == -1) GD::out.printError("Could not set owner on " + file);
					if(chmod(file.c_str(), GD::bl->settings.dataPathPermissions()) == -1) GD::out.printError("Could not set permissions on " + file);
				}
			}

			if(runningAsUser)
			{
				//Logs are created as root. So it is really important to set the permissions here.
				if(chown((GD::bl->settings.logfilePath() + "homegear.log").c_str(), GD::bl->userId, GD::bl->groupId) == -1) GD::out.printError("Could not set owner on file homegear.log");
				if(chown((GD::bl->settings.logfilePath() + "homegear.err").c_str(), GD::bl->userId, GD::bl->groupId) == -1) GD::out.printError("Could not set owner on file homegear.err");
			}
        }

    	GD::licensingController->loadModules();

		GD::familyController->loadModules();

		bindRPCServers();

    	if(getuid() == 0 && !GD::runAsUser.empty() && !GD::runAsGroup.empty())
    	{
			if(GD::bl->userId == 0 || GD::bl->groupId == 0)
			{
				GD::out.printCritical("Could not drop privileges. User name or group name is not valid.");
				exitHomegear(1);
			}
			GD::out.printInfo("Info: Setting up physical interfaces and GPIOs...");
			if(GD::familyController) GD::familyController->physicalInterfaceSetup(GD::bl->userId, GD::bl->groupId, GD::bl->settings.setDevicePermissions());
			BaseLib::LowLevel::Gpio gpio(GD::bl.get());
			gpio.setup(GD::bl->userId, GD::bl->groupId, GD::bl->settings.setDevicePermissions());
			GD::out.printInfo("Info: Dropping privileges to user " + GD::runAsUser + " (" + std::to_string(GD::bl->userId) + ") and group " + GD::runAsGroup + " (" + std::to_string(GD::bl->groupId) + ")");

			int result = -1;
			std::vector<gid_t> supplementaryGroups(10);
			int numberOfGroups = 10;
			while(result == -1)
			{
				result = getgrouplist(GD::runAsUser.c_str(), 10000, supplementaryGroups.data(), &numberOfGroups);

				if(result == -1) supplementaryGroups.resize(numberOfGroups);
				else supplementaryGroups.resize(result);
			}

			if(setgid(GD::bl->groupId) != 0)
			{
				GD::out.printCritical("Critical: Could not drop group privileges.");
				exitHomegear(1);
			}

			if(setgroups(supplementaryGroups.size(), supplementaryGroups.data()) != 0)
			{
				GD::out.printCritical("Critical: Could not set supplementary groups: " + std::string(strerror(errno)));
				exitHomegear(1);
			}

			if(setuid(GD::bl->userId) != 0)
			{
				GD::out.printCritical("Critical: Could not drop user privileges.");
				exitHomegear(1);
			}

			//Core dumps are disabled by setuid. Enable them again.
			if(GD::bl->settings.enableCoreDumps()) prctl(PR_SET_DUMPABLE, 1);
    	}

    	if(getuid() == 0)
    	{
    		if(!GD::runAsUser.empty() && !GD::runAsGroup.empty())
    		{
    			GD::out.printCritical("Critical: Homegear still has root privileges though privileges should have been dropped. Exiting Homegear as this is a security risk.");
				exit(1);
    		}
    		else GD::out.printWarning("Warning: Running as root. The authors of Homegear recommend running Homegear as user.");
    	}
    	else
    	{
    		if(setuid(0) != -1)
			{
				GD::out.printCritical("Critical: Regaining root privileges succeded. Exiting Homegear as this is a security risk.");
				exit(1);
			}
    		GD::out.printInfo("Info: Homegear is (now) running as user with id " + std::to_string(getuid()) + " and group with id " + std::to_string(getgid()) + '.');
    	}

    	//Create PID file
    	try
    	{
			if(!GD::pidfilePath.empty())
			{
				int32_t pidfile = open(GD::pidfilePath.c_str(), O_CREAT | O_RDWR, 0666);
				if(pidfile < 0)
				{
					GD::out.printError("Error: Cannot create pid file \"" + GD::pidfilePath + "\".");
				}
				else
				{
					int32_t rc = flock(pidfile, LOCK_EX | LOCK_NB);
					if(rc && errno == EWOULDBLOCK)
					{
						GD::out.printError("Error: Homegear is already running - Can't lock PID file.");
					}
					std::string pid(std::to_string(getpid()));
					int32_t bytesWritten = write(pidfile, pid.c_str(), pid.size());
					if(bytesWritten <= 0) GD::out.printError("Error writing to PID file: " + std::string(strerror(errno)));
					close(pidfile);
				}
			}
		}
		catch(const std::exception& ex)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
		}
		catch(BaseLib::Exception& ex)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
		}
		catch(...)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
		}

		if(!GD::bl->io.directoryExists(GD::bl->settings.tempPath()))
		{
			if(!GD::bl->io.createDirectory(GD::bl->settings.tempPath(), S_IRWXU | S_IRWXG))
			{
				GD::out.printCritical("Critical: Cannot create temp directory \"" + GD::bl->settings.tempPath());
				exit(1);
			}
		}
		std::vector<std::string> tempFiles = GD::bl->io.getFiles(GD::bl->settings.tempPath(), false);
		for(std::vector<std::string>::iterator i = tempFiles.begin(); i != tempFiles.end(); ++i)
		{
			if(!GD::bl->io.deleteFile(GD::bl->settings.tempPath() + *i))
			{
				GD::out.printCritical("Critical: deleting temporary file \"" + GD::bl->settings.tempPath() + *i + "\": " + strerror(errno));
			}
		}
		std::string phpTempPath = GD::bl->settings.tempPath() + "/php/";
		if(GD::bl->io.directoryExists(phpTempPath))
		{
			tempFiles = GD::bl->io.getFiles(phpTempPath, false);
			for(std::vector<std::string>::iterator i = tempFiles.begin(); i != tempFiles.end(); ++i)
			{
				if(!GD::bl->io.deleteFile(phpTempPath + *i))
				{
					GD::out.printCritical("Critical: deleting temporary file \"" + phpTempPath + *i + "\": " + strerror(errno));
				}
			}
		}

		while(BaseLib::HelperFunctions::getTime() < 1000000000000)
		{
			GD::out.printWarning("Warning: Time is in the past. Waiting for ntp to set the time...");
			std::this_thread::sleep_for(std::chrono::milliseconds(10000));
		}

#ifdef EVENTHANDLER
		GD::eventHandler.reset(new EventHandler());
#endif

		GD::flowsServer.reset(new Flows::FlowsServer());
		GD::ipcServer.reset(new Ipc::IpcServer());

		if(!GD::bl->io.directoryExists(GD::bl->settings.tempPath() + "php"))
		{
			if(!GD::bl->io.createDirectory(GD::bl->settings.tempPath() + "php", S_IRWXU | S_IRWXG))
			{
				GD::out.printCritical("Critical: Cannot create temp directory \"" + GD::bl->settings.tempPath() + "php");
				exitHomegear(1);
			}
		}
#ifndef NO_SCRIPTENGINE
		GD::out.printInfo("Starting script engine server...");
		GD::scriptEngineServer.reset(new ScriptEngine::ScriptEngineServer());
		if(!GD::scriptEngineServer->start())
		{
			GD::out.printCritical("Critical: Cannot start script engine server. Exiting Homegear.");
			exitHomegear(1);
		}
#else
		GD::out.printInfo("Info: Homegear is compiled without script engine.");
#endif

        GD::out.printInfo("Initializing licensing controller...");
        GD::licensingController->init();

        GD::out.printInfo("Loading licensing controller data...");
        GD::licensingController->load();

        GD::out.printInfo("Loading devices...");
        if(BaseLib::Io::fileExists(GD::configPath + "physicalinterfaces.conf")) GD::out.printWarning("Warning: File physicalinterfaces.conf exists in config directory. Interface configuration has been moved to " + GD::bl->settings.familyConfigPath());
        GD::familyController->load(); //Don't load before database is open!

        GD::out.printInfo("Initializing RPC client...");
        GD::rpcClient->init();

        if(GD::mqtt->enabled())
		{
			GD::out.printInfo("Starting MQTT client...");;
			GD::mqtt->start();
		}

        startRPCServers();

		GD::out.printInfo("Starting CLI server...");
		GD::cliServer.reset(new CLI::Server());
		GD::cliServer->start();

#ifdef EVENTHANDLER
        GD::out.printInfo("Initializing event handler...");
        GD::eventHandler->init();
        GD::out.printInfo("Loading events...");
        GD::eventHandler->load();
#endif

		if(GD::bl->settings.enableFlows())
		{
			GD::out.printInfo("Starting flows server...");
			if(!GD::flowsServer->start())
			{
				GD::out.printCritical("Critical: Cannot start flows server. Exiting Homegear.");
				exitHomegear(1);
			}
		}

		GD::out.printInfo("Starting IPC server...");
		if(!GD::ipcServer->start())
		{
			GD::out.printCritical("Critical: Cannot start IPC server. Exiting Homegear.");
			exitHomegear(1);
		}

		GD::out.printInfo("Start listening for packets...");
        GD::familyController->physicalInterfaceStartListening();
        if(!GD::familyController->physicalInterfaceIsOpen())
        {
        	GD::out.printCritical("Critical: At least one of the physical devices could not be opened... Exiting...");
        	GD::familyController->physicalInterfaceStopListening();
        	exitHomegear(1);
        }

        GD::out.printMessage("Startup complete. Waiting for physical interfaces to connect.");

        //Wait for all interfaces to connect before setting booting to false
        {
			for(int32_t i = 0; i < 300; i++)
			{
				if(GD::familyController->physicalInterfaceIsOpen())
				{
					GD::out.printMessage("All physical interfaces are connected now.");
					break;
				}
				if(i == 299) GD::out.printError("Error: At least one physical interface is not connected.");
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			}
        }

        if(GD::bl->settings.enableUPnP())
		{
        	GD::out.printInfo("Starting UPnP server...");
        	GD::uPnP->start();
		}

        GD::bl->booting = false;
        GD::familyController->homegearStarted();

        _shuttingDownMutex.lock();
		_startUpComplete = true;
		if(_shutdownQueued)
		{
			_shuttingDownMutex.unlock();
			terminate(SIGTERM);
		}
		_shuttingDownMutex.unlock();

		if(BaseLib::Io::fileExists(GD::bl->settings.workingDirectory() + "core"))
		{
			GD::out.printError("Error: A core file exists in Homegear's working directory (\"" + GD::bl->settings.workingDirectory() + "core" + "\"). Please send this file to the Homegear team including information about your system (Linux distribution, CPU architecture), the Homegear version, the current log files and information what might've caused the error.");
		}

		char* inputBuffer = nullptr;
        if(_startAsDaemon || _nonInteractive)
        {
        	while(true) std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        else
        {
        	rl_bind_key('\t', rl_abort); //no autocompletion
			while(true)
			{
				inputBuffer = readline("");
				if(inputBuffer == nullptr)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));
					continue;
				}
				if(inputBuffer[0] == '\n' || inputBuffer[0] == 0) continue;
				if(strncmp(inputBuffer, "quit", 4) == 0 || strncmp(inputBuffer, "exit", 4) == 0 || strncmp(inputBuffer, "moin", 4) == 0) break;

				add_history(inputBuffer); //Sets inputBuffer to 0

				std::string input(inputBuffer);
				std::cout << GD::cliServer->handleCommand(input);
				free(inputBuffer);
			}
			clear_history();
        }

        terminate(SIGTERM);
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

int main(int argc, char* argv[])
{
	try
    {
		_reloading = false;
		_startUpComplete = false;
		_shutdownQueued = false;
		_monitorProcess = false;

    	getExecutablePath(argc, argv);
    	_errorCallback.reset(new std::function<void(int32_t, std::string)>(errorCallback));
    	GD::bl.reset(new BaseLib::SharedObjects());
    	GD::out.init(GD::bl.get());

		if(BaseLib::Io::directoryExists(GD::executablePath + "config")) GD::configPath = GD::executablePath + "config/";
		else if(BaseLib::Io::directoryExists(GD::executablePath + "cfg")) GD::configPath = GD::executablePath + "cfg/";
		else GD::configPath = "/etc/homegear/";

    	if(std::string(VERSION) != GD::bl->version())
    	{
    		GD::out.printCritical(std::string("Base library has wrong version. Expected version ") + VERSION + " but got version " + GD::bl->version());
    		exit(1);
    	}

    	for(int32_t i = 1; i < argc; i++)
    	{
    		std::string arg(argv[i]);
    		if(arg == "-h" || arg == "--help")
    		{
    			printHelp();
    			exit(0);
    		}
    		else if(arg == "-c")
    		{
    			if(i + 1 < argc)
    			{
    				std::string configPath = std::string(argv[i + 1]);
    				if(!configPath.empty()) GD::configPath = configPath;
    				if(GD::configPath[GD::configPath.size() - 1] != '/') GD::configPath.push_back('/');
    				i++;
    			}
    			else
    			{
    				printHelp();
    				exit(1);
    			}
    		}
    		else if(arg == "-p")
    		{
    			if(i + 1 < argc)
    			{
    				GD::pidfilePath = std::string(argv[i + 1]);
    				i++;
    			}
    			else
    			{
    				printHelp();
    				exit(1);
    			}
    		}
    		else if(arg == "-u")
    		{
    			if(i + 1 < argc)
    			{
    				GD::runAsUser = std::string(argv[i + 1]);
    				i++;
    			}
    			else
    			{
    				printHelp();
    				exit(1);
    			}
    		}
    		else if(arg == "-g")
    		{
    			if(i + 1 < argc)
    			{
    				GD::runAsGroup = std::string(argv[i + 1]);
    				i++;
    			}
    			else
    			{
    				printHelp();
    				exit(1);
    			}
    		}
    		else if(arg == "-s")
    		{
    			if(i + 2 < argc)
    			{
    				if(getuid() != 0)
    				{
    					std::cout <<  "Please run Homegear as root to set the device permissions." << std::endl;
    					exit(1);
    				}
    				GD::bl->settings.load(GD::configPath + "main.conf", GD::executablePath);
    				GD::bl->debugLevel = 3; //Only output warnings.
    				GD::licensingController.reset(new LicensingController());
    				GD::familyController.reset(new FamilyController());
    				GD::licensingController->loadModules();
    				GD::familyController->loadModules();
    				uid_t userId = GD::bl->hf.userId(std::string(argv[i + 1]));
    				gid_t groupId = GD::bl->hf.groupId(std::string(argv[i + 2]));
    				GD::out.printDebug("Debug: User ID set to " + std::to_string(userId) + " group ID set to " + std::to_string(groupId));
    				if((signed)userId == -1 || (signed)groupId == -1)
    				{
    					GD::out.printCritical("Could not setup physical devices. Username or group name is not valid.");
    					GD::familyController->dispose();
    					GD::licensingController->dispose();
    					exit(1);
    				}
    				GD::familyController->physicalInterfaceSetup(userId, groupId, GD::bl->settings.setDevicePermissions());
    				BaseLib::LowLevel::Gpio gpio(GD::bl.get());
    				gpio.setup(userId, groupId, GD::bl->settings.setDevicePermissions());
    				GD::familyController->dispose();
    				GD::licensingController->dispose();
    				exit(0);
    			}
    			else
    			{
    				printHelp();
    				exit(1);
    			}
    		}
    		else if(arg == "-o")
    		{
    			if(i + 2 < argc)
    			{
    				GD::bl->settings.load(GD::configPath + "main.conf", GD::executablePath);
    				std::string inputFile(argv[i + 1]);
    				std::string outputFile(argv[i + 2]);
    				BaseLib::DeviceDescription::Devices devices(GD::bl.get(), nullptr, 0);
					std::shared_ptr<HomegearDevice> device = devices.loadFile(inputFile);
					if(!device) exit(1);
					device->save(outputFile);
    				exit(0);
    			}
    			else
    			{
    				printHelp();
    				exit(1);
    			}
    		}
    		else if(arg == "-d")
    		{
    			_startAsDaemon = true;
    		}
    		else if(arg == "-r")
    		{
    			GD::bl->settings.load(GD::configPath + "main.conf", GD::executablePath);
    			CLI::Client cliClient;
    			int32_t exitCode = cliClient.start();
    			exit(exitCode);
    		}
#ifndef NO_SCRIPTENGINE
    		else if(arg == "-rse")
    		{
    			GD::bl->settings.load(GD::configPath + "main.conf", GD::executablePath);
    			initGnuTls();
    			setLimits();
    			GD::licensingController.reset(new LicensingController());
    			GD::licensingController->loadModules();
    			GD::licensingController->init();
    			GD::licensingController->load();
    			ScriptEngine::ScriptEngineClient scriptEngineClient;
    			scriptEngineClient.start();
    			GD::licensingController->dispose();
    			exit(0);
    		}
#endif
    		else if(arg == "-rl")
    		{
    			GD::bl->settings.load(GD::configPath + "main.conf", GD::executablePath);
    			initGnuTls();
    			setLimits();
    			GD::licensingController.reset(new LicensingController());
    			GD::licensingController->loadModules();
    			GD::licensingController->init();
    			GD::licensingController->load();
    			Flows::FlowsClient flowsClient;
    			flowsClient.start();
    			GD::licensingController->dispose();
    			exit(0);
    		}
    		else if(arg == "-e")
    		{
    			GD::bl->settings.load(GD::configPath + "main.conf", GD::executablePath);
    			GD::bl->debugLevel = 3; //Only output warnings.
    			std::stringstream command;
    			if(i + 1 < argc)
    			{
    				command << std::string(argv[i + 1]);
    			}
    			else
    			{
    				printHelp();
    				exit(1);
    			}

    			for(int32_t j = i + 2; j < argc; j++)
    			{
    				std::string element(argv[j]);
    				if(element.find(' ') != std::string::npos) command << " \"" << element << "\"";
    				else command << " " << argv[j];
    			}

    			CLI::Client cliClient;
    			int32_t exitCode = cliClient.start(command.str());
    			exit(exitCode);
    		}
    		else if(arg == "-tc")
    		{
    			GD::bl->threadManager.testMaxThreadCount();
    			std::cout << GD::bl->threadManager.getMaxThreadCount() << std::endl;
    			exit(0);
    		}
    		else if(arg == "-l")
    		{
    			GD::bl->settings.load(GD::configPath + "main.conf", GD::executablePath);
    			GD::bl->debugLevel = 3; //Only output warnings.
    			std::string command = "lifetick";
    			CLI::Client cliClient;
    			int32_t exitCode = cliClient.start(command);
    			exit(exitCode);
    		}
    		else if(arg == "-pre")
    		{
    			GD::bl->settings.load(GD::configPath + "main.conf", GD::executablePath);
    			GD::serverInfo.init(GD::bl.get());
    			GD::serverInfo.load(GD::bl->settings.serverSettingsPath());
    			if(GD::runAsUser.empty()) GD::runAsUser = GD::bl->settings.runAsUser();
				if(GD::runAsGroup.empty()) GD::runAsGroup = GD::bl->settings.runAsGroup();
				if((!GD::runAsUser.empty() && GD::runAsGroup.empty()) || (!GD::runAsGroup.empty() && GD::runAsUser.empty()))
				{
					GD::out.printCritical("Critical: You only provided a user OR a group for Homegear to run as. Please specify both.");
					exit(1);
				}
				if(GD::runAsUser.empty() || GD::runAsGroup.empty())
				{
					GD::out.printInfo("Info: Not setting permissions as user and group are not specified.");
					exit(0);
				}
				uid_t userId = GD::bl->hf.userId(GD::runAsUser);
				gid_t groupId = GD::bl->hf.groupId(GD::runAsGroup);
				std::string currentPath;
    			if(!GD::pidfilePath.empty() && GD::pidfilePath.find('/') != std::string::npos)
    			{
    				currentPath = GD::pidfilePath.substr(0, GD::pidfilePath.find_last_of('/'));
    				if(!currentPath.empty())
    				{
    					if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, S_IRWXU | S_IRWXG);
    					if(chown(currentPath.c_str(), userId, groupId) == -1) std::cerr << "Could not set owner on " << currentPath << std::endl;
    					if(chmod(currentPath.c_str(), S_IRWXU | S_IRWXG) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    				}
    			}

    			currentPath = GD::bl->settings.dataPath();
    			if(!currentPath.empty() && currentPath != GD::executablePath)
				{
    				uid_t localUserId = GD::bl->hf.userId(GD::bl->settings.dataPathUser());
					gid_t localGroupId = GD::bl->hf.groupId(GD::bl->settings.dataPathGroup());
					if(((int32_t)localUserId) == -1 || ((int32_t)localGroupId) == -1)
					{
						localUserId = userId;
						localGroupId = groupId;
					}
					if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, GD::bl->settings.dataPathPermissions());
					if(chown(currentPath.c_str(), localUserId, localGroupId) == -1) std::cerr << "Could not set owner on " << currentPath << std::endl;
					if(chmod(currentPath.c_str(), GD::bl->settings.dataPathPermissions()) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
					std::vector<std::string> subdirs = GD::bl->io.getDirectories(currentPath, true);
					for(std::vector<std::string>::iterator j = subdirs.begin(); j != subdirs.end(); ++j)
					{
						std::string subdir = currentPath + *j;
						if(subdir != GD::bl->settings.scriptPath() && subdir != GD::bl->settings.flowsPath() && subdir != GD::bl->settings.flowsDataPath() && subdir != GD::bl->settings.socketPath() && subdir != GD::bl->settings.modulePath() && subdir != GD::bl->settings.logfilePath())
						{
							if(chown(subdir.c_str(), localUserId, localGroupId) == -1) std::cerr << "Could not set owner on " << subdir << std::endl;
						}
						std::vector<std::string> files = GD::bl->io.getFiles(subdir, false);
						for(std::vector<std::string>::iterator k = files.begin(); k != files.end(); ++k)
						{
							std::string file = subdir + *k;
							if(chown(file.c_str(), localUserId, localGroupId) == -1) std::cerr << "Could not set owner on " << file << std::endl;
						}
					}
					for(std::vector<std::string>::iterator j = subdirs.begin(); j != subdirs.end(); ++j)
					{
						std::string subdir = currentPath + *j;
						if(subdir == GD::bl->settings.scriptPath() || subdir == GD::bl->settings.flowsPath() || subdir == GD::bl->settings.flowsDataPath() || subdir == GD::bl->settings.socketPath() || subdir == GD::bl->settings.modulePath() || subdir == GD::bl->settings.logfilePath()) continue;
						if(chmod(subdir.c_str(), GD::bl->settings.dataPathPermissions()) == -1) std::cerr << "Could not set permissions on " << subdir << std::endl;
					}
				}

    			std::string databasePath = (GD::bl->settings.databasePath().empty() ? GD::bl->settings.dataPath() : GD::bl->settings.databasePath()) + "db.sql";
    			if(BaseLib::Io::fileExists(databasePath))
    			{
    				if(chmod(databasePath.c_str(), S_IRUSR | S_IWUSR | S_IRGRP) == -1) std::cerr << "Could not set permissions on " << databasePath << std::endl;
    			}

    			currentPath = GD::bl->settings.scriptPath();
    			if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, S_IRWXU | S_IRWXG);
    			uid_t localUserId = GD::bl->hf.userId(GD::bl->settings.scriptPathUser());
				gid_t localGroupId = GD::bl->hf.groupId(GD::bl->settings.scriptPathGroup());
				if(((int32_t)localUserId) == -1 || ((int32_t)localGroupId) == -1)
				{
					localUserId = userId;
					localGroupId = groupId;
				}
				if(chown(currentPath.c_str(), localUserId, localGroupId) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    			if(chmod(currentPath.c_str(), GD::bl->settings.scriptPathPermissions()) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;

    			currentPath = GD::bl->settings.flowsPath();
    			if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, S_IRWXU | S_IRWXG);
    			localUserId = GD::bl->hf.userId(GD::bl->settings.flowsPathUser());
				localGroupId = GD::bl->hf.groupId(GD::bl->settings.flowsPathGroup());
				if(((int32_t)localUserId) == -1 || ((int32_t)localGroupId) == -1)
				{
					localUserId = userId;
					localGroupId = groupId;
				}
				if(chown(currentPath.c_str(), localUserId, localGroupId) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    			if(chmod(currentPath.c_str(), GD::bl->settings.flowsPathPermissions()) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;

    			currentPath = GD::bl->settings.flowsPath() + "nodes/";
    			if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, S_IRWXU | S_IRWXG);
    			localUserId = GD::bl->hf.userId(GD::bl->settings.flowsPathUser());
				localGroupId = GD::bl->hf.groupId(GD::bl->settings.flowsPathGroup());
				if(((int32_t)localUserId) == -1 || ((int32_t)localGroupId) == -1)
				{
					localUserId = userId;
					localGroupId = groupId;
				}
				if(chown(currentPath.c_str(), localUserId, localGroupId) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    			if(chmod(currentPath.c_str(), GD::bl->settings.flowsPathPermissions()) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;

    			currentPath = GD::bl->settings.flowsDataPath();
    			if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, S_IRWXU | S_IRWXG);
    			localUserId = GD::bl->hf.userId(GD::bl->settings.flowsPathUser());
				localGroupId = GD::bl->hf.groupId(GD::bl->settings.flowsPathGroup());
				if(((int32_t)localUserId) == -1 || ((int32_t)localGroupId) == -1)
				{
					localUserId = userId;
					localGroupId = groupId;
				}
				if(chown(currentPath.c_str(), localUserId, localGroupId) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    			if(chmod(currentPath.c_str(), GD::bl->settings.flowsPathPermissions()) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;

    			if(GD::bl->settings.socketPath() != GD::bl->settings.dataPath() && GD::bl->settings.socketPath() != GD::executablePath)
    			{
					currentPath = GD::bl->settings.socketPath();
					if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, S_IRWXU | S_IRWXG);
					if(chown(currentPath.c_str(), userId, groupId) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
					if(chmod(currentPath.c_str(), S_IRWXU | S_IRWXG) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    			}

    			if(GD::bl->settings.lockFilePath() != GD::bl->settings.dataPath() && GD::bl->settings.lockFilePath() != GD::executablePath)
    			{
    				uid_t localUserId = GD::bl->hf.userId(GD::bl->settings.lockFilePathUser());
					gid_t localGroupId = GD::bl->hf.groupId(GD::bl->settings.lockFilePathGroup());
					currentPath = GD::bl->settings.lockFilePath();
					if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, S_IRWXU | S_IRWXG);
					if(((int32_t)localUserId) != -1 && ((int32_t)localGroupId) != -1) { if(chown(currentPath.c_str(), localUserId, localGroupId) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl; }
					if(GD::bl->settings.lockFilePathPermissions() != 0) { if(chmod(currentPath.c_str(), GD::bl->settings.lockFilePathPermissions()) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl; }
    			}

    			currentPath = GD::bl->settings.modulePath();
    			if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP);
    			if(chown(currentPath.c_str(), userId, groupId) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    			if(chmod(currentPath.c_str(), S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    			std::vector<std::string> files = GD::bl->io.getFiles(currentPath, false);
				for(std::vector<std::string>::iterator j = files.begin(); j != files.end(); ++j)
				{
					std::string file = currentPath + *j;
					if(chown(file.c_str(), userId, groupId) == -1) std::cerr << "Could not set owner on " << file << std::endl;
				}

    			currentPath = GD::bl->settings.logfilePath();
    			if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, S_IRWXU | S_IRGRP | S_IXGRP);
    			if(chown(currentPath.c_str(), userId, groupId) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    			if(chmod(currentPath.c_str(), S_IRWXU | S_IRGRP | S_IXGRP) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
    			files = GD::bl->io.getFiles(currentPath, false);
				for(std::vector<std::string>::iterator j = files.begin(); j != files.end(); ++j)
				{
					std::string file = currentPath + *j;
					if(chown(file.c_str(), userId, groupId) == -1) std::cerr << "Could not set owner on " << file << std::endl;
					if(chmod(file.c_str(), S_IRUSR | S_IWUSR | S_IRGRP) == -1) std::cerr << "Could not set permissions on " << file << std::endl;
				}

				for(int32_t i = 0; i < GD::serverInfo.count(); i++)
				{
					BaseLib::Rpc::PServerInfo settings = GD::serverInfo.get(i);
					if(settings->contentPathUser.empty() || settings->contentPathGroup.empty()) continue;
					uid_t localUserId = GD::bl->hf.userId(settings->contentPathUser);
					gid_t localGroupId = GD::bl->hf.groupId(settings->contentPathGroup);
					if(((int32_t)localUserId) == -1 || ((int32_t)localGroupId) == -1) continue;
					currentPath = settings->contentPath;
					if(!BaseLib::Io::directoryExists(currentPath)) BaseLib::Io::createDirectory(currentPath, settings->contentPathPermissions);
					if(chown(currentPath.c_str(), localUserId, localGroupId) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
					if(chmod(currentPath.c_str(), settings->contentPathPermissions) == -1) std::cerr << "Could not set permissions on " << currentPath << std::endl;
				}

    			exit(0);
    		}
    		else if(arg == "-v")
    		{
    			std::cout << "Homegear version " << VERSION << std::endl;
    			std::cout << "Copyright (c) 2013-2017 Sathya Laufer" << std::endl << std::endl;
    			std::cout << "Git commit SHA of libhomegear-base: " << GITCOMMITSHABASE << std::endl;
    			std::cout << "Git branch of libhomegear-base:     " << GITBRANCHBASE << std::endl;
    			std::cout << "Git commit SHA of Homegear:         " << GITCOMMITSHAHOMEGEAR << std::endl;
    			std::cout << "Git branch of Homegear:             " << GITBRANCHHOMEGEAR << std::endl << std::endl;
    			std::cout << "PHP (License: PHP License):" << std::endl;
    			std::cout << "This product includes PHP software, freely available from <http://www.php.net/software/>" << std::endl;
    			std::cout << "Copyright (c) 1999-2017 The PHP Group. All rights reserved." << std::endl << std::endl;

    			exit(0);
    		}
    		else
    		{
    			printHelp();
    			exit(1);
    		}
    	}

    	if(!isatty(STDIN_FILENO)) _nonInteractive = true;

    	try
    	{
    		// {{{ Get maximum thread count
				std::string output;
				BaseLib::HelperFunctions::exec(GD::executablePath + GD::executableFile + " -tc", output);
				BaseLib::HelperFunctions::trim(output);
				if(BaseLib::Math::isNumber(output, false)) GD::bl->threadManager.setMaxThreadCount(BaseLib::Math::getNumber(output, false));
			// }}}
		}
		catch(const std::exception& ex)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
		}
		catch(BaseLib::Exception& ex)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
		}
		catch(...)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
		}

    	// {{{ Load settings
			GD::out.printInfo("Loading settings from " + GD::configPath + "main.conf");
			GD::bl->settings.load(GD::configPath + "main.conf", GD::executablePath);
			if(GD::runAsUser.empty()) GD::runAsUser = GD::bl->settings.runAsUser();
			if(GD::runAsGroup.empty()) GD::runAsGroup = GD::bl->settings.runAsGroup();
			if((!GD::runAsUser.empty() && GD::runAsGroup.empty()) || (!GD::runAsGroup.empty() && GD::runAsUser.empty()))
			{
				GD::out.printCritical("Critical: You only provided a user OR a group for Homegear to run as. Please specify both.");
				exit(1);
			}
			GD::bl->userId = GD::bl->hf.userId(GD::runAsUser);
			GD::bl->groupId = GD::bl->hf.groupId(GD::runAsGroup);
			if((int32_t)GD::bl->userId == -1 || (int32_t)GD::bl->groupId == -1)
			{
				GD::bl->userId = 0;
				GD::bl->groupId = 0;
			}

			GD::out.printInfo("Loading RPC server settings from " + GD::bl->settings.serverSettingsPath());
			GD::serverInfo.init(GD::bl.get());
			GD::serverInfo.load(GD::bl->settings.serverSettingsPath());
			GD::out.printInfo("Loading RPC client settings from " + GD::bl->settings.clientSettingsPath());
			GD::clientSettings.load(GD::bl->settings.clientSettingsPath());
			GD::mqtt.reset(new Mqtt());
			GD::mqtt->loadSettings();
		// }}}

		if((chdir(GD::bl->settings.workingDirectory().c_str())) < 0)
		{
			GD::out.printError("Could not change working directory to " + GD::bl->settings.workingDirectory() + ".");
			exitHomegear(1);
		}

		GD::licensingController.reset(new LicensingController());
		GD::familyController.reset(new FamilyController());
		GD::bl->db.reset(new DatabaseController());
		GD::rpcClient.reset(new Rpc::Client());

    	if(_startAsDaemon) startDaemon();
    	startUp();

        return 0;
    }
    catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(BaseLib::Exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	terminate(SIGTERM);

    return 1;
}

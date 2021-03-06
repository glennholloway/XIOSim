import os
import shlex
import subprocess

class XIOSimDriver(object):
    def __init__(self, PIN_ROOT, INSTALL_DIR, TREE_DIR, clean_arch=None, env=None):
        self.cmd = ""
        self.PIN = os.path.join(PIN_ROOT, "pin.sh")
        self.INSTALL_DIR = INSTALL_DIR
        self.TREE_DIR = TREE_DIR
        if clean_arch:
            self.AddCleanArch()
        if env:
            self.AddEnvironment(env)
        self.AddHarness()

    def AddCleanArch(self):
        self.cmd += "/usr/bin/setarch i686 -BR "

    def AddEnvironment(self, env):
        self.cmd += "/usr/bin/env -i " + env + " "

    def AddHarness(self):
        self.cmd +=  os.path.join(self.INSTALL_DIR, "harness") + " "

    def AddBmks(self, bmk_cfg):
        self.cmd += "-benchmark_cfg " + bmk_cfg + " "

    def AddPinOptions(self):
        self.cmd += "-pin " + self.PIN + " "
        self.cmd += "-xyzzy "
        self.cmd += "-pause_tool 1 "
        self.cmd += "-t " + os.path.join(self.INSTALL_DIR, "feeder_zesto.so") + " "

    def AddPintoolOptions(self, num_cores):
        self.cmd += "-num_cores %d " % num_cores

    def AddPinPointFile(self, file):
        self.cmd += "-ppfile %s " % file

    def AddInstLength(self, ninsn):
        self.cmd += "-length %d " % ninsn

    def AddSkipInst(self, ninsn):
        self.cmd += "-skip %d " % ninsn

    def AddMolecoolOptions(self):
        self.cmd += "-ildjit "

    def AddTraceFile(self, file):
        self.cmd += "-trace %s " % file

    def AddZestoOptions(self, cfg, mem_cfg=None):
        self.cmd += "-s "
        self.cmd += "-config " + cfg + " "
        if mem_cfg:
            self.cmd += "-config " + mem_cfg + " "

    def AddZestoOut(self, ofile):
        self.cmd += "-redir:sim " + ofile + " "

    def AddZestoHeartbeat(self, ncycles):
        self.cmd += "-heartbeat " + str(ncycles) + " "

    def AddZestoCores(self, ncores):
        self.cmd += "-cores " + str(ncores) + " "

    def AddZestoPowerFile(self, fname):
        self.cmd += "-power:rtp_file " + fname + " "

    def AddILDJITOptions(self):
        self.cmd += "-- iljit --static -O3 -M -N -R -T "

    def Exec(self, stdin_file=None, stdout_file=None, stderr_file=None, cwd=None):
        print self.cmd

        if cwd:
            self.run_dir = cwd

        # Provide input/output redirection
        if stdin_file:
            stdin = open(stdin_file, "r")
        else:
            stdin = None

        if stdout_file:
            stdout = open(stdout_file, "w")
        else:
            stdout = None

        if stderr_file:
            stderr=open(stderr_file, "w")
        else:
            stderr = None

        #... and finally launch command
        child = subprocess.Popen(shlex.split(self.cmd), close_fds=True, stdin=stdin, stdout=stdout, stderr=stderr, cwd=cwd)
        retcode = child.wait()

        if retcode == 0:
            print "Completed"
        else:
            print "Failed! Error code: %d" % retcode
        return retcode

    def GetRunDir(self):
        return self.run_dir

    def GetSimOut(self):
        return os.path.join(self.GetRunDir(), "sim.out")

    def GetTreeDir(self):
        return self.TREE_DIR

    def GenerateTestConfig(self, test):
        res = []
        res.append("program {\n")
        res.append("  exe = \"%s\"\n" % os.path.join(self.TREE_DIR, "tests", test))
        res.append("  args = \"> %s.out 2> %s.err\"\n" % (test, test))
        res.append("  instances = 1\n")
        res.append("}\n")
        return res

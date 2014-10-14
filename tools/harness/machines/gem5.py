##########################################################################
# Copyright (c) 2012, ETH Zurich.
# All rights reserved.
#
# This file is distributed under the terms in the attached LICENSE file.
# If you do not find this file, copies can be found by writing to:
# ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
##########################################################################


# Quirks:
# * menu.lst is read from hake/menu.lst.arm_gem5, not the one 
#    generated by scalebench
# * this is only running in single-core mode, since bootarm=0 is
#    used in above mentioned menu.lst

import os, signal, tempfile, subprocess, shutil, time
import debug, machines
from machines import Machine

GEM5_PATH = '/home/netos/tools/gem5/gem5/build/ARM/'
GEM5_CACHES_ENABLE = '--caches --l2cache'.split()
# gem5 takes quite a while to come up. If we return right away, 
# telnet will be opened too early and fails to connect
GEM5_START_TIMEOUT = 90 # in seconds


class Gem5MachineBase(Machine):
	def __init__(self, options):
		super(Gem5MachineBase, self).__init__(options)
		self.child = None
		self.telnet = None
		self.tftp_dir = None
		self.options = options
        
	def get_coreids(self):
		return range(0, self.get_ncores())
        
	def get_tickrate(self):
		return None
        
	def get_boot_timeout(self):
		# we set this to 10 mins since gem5 is very slow
		return 600
        
	def get_tftp_dir(self):
		if self.tftp_dir is None:
			debug.verbose('creating temporary directory for Gem5 files')
			self.tftp_dir = tempfile.mkdtemp(prefix='harness_gem5_')
			debug.verbose('Gem5 install directory is %s' % self.tftp_dir)
		return self.tftp_dir

	# Use menu.lst in hake/menu.lst.arm_gem5
	def _write_menu_lst(self, data, path):
		pass
		
	def set_bootmodules(self, modules):
		pass
	
	def lock(self):
		pass

	def unlock(self):
		pass

	def setup(self):
		pass

	def _get_cmdline(self):
		raise NotImplementedError

	def _kill_child(self):
		# terminate child if running
		if self.child:
                    try:
			os.kill(self.child.pid, signal.SIGTERM)
                    except OSError, e:
                        debug.verbose("Caught OSError trying to kill child: %r" % e)
                    except Exception, e:
                        debug.verbose("Caught exception trying to kill child: %r" % e)
                    try:
                        self.child.wait()
                    except Exception, e:
                        debug.verbose("Caught exception while waiting for child: %r" % e)
                    self.child = None
			
	def reboot(self):
		self._kill_child()
		cmd = self._get_cmdline()
		debug.verbose('starting "%s" in gem5.py:reboot' % ' '.join(cmd))
		devnull = open('/dev/null', 'w')
		self.child = subprocess.Popen(cmd, stderr=devnull)
		time.sleep(GEM5_START_TIMEOUT)

	def shutdown(self):
		debug.verbose('gem5:shutdown requested');
		debug.verbose('terminating gem5')
		if not self.child is None:
                    try:
                        self.child.terminate()
                    except OSError, e:
                        debug.verbose("Error when trying to terminate gem5: %r" % e)
		debug.verbose('terminating telnet')
		if not self.telnet is None:
                        self.telnet.terminate()
		# try to cleanup tftp tree if needed
		if self.tftp_dir and os.path.isdir(self.tftp_dir):
			shutil.rmtree(self.tftp_dir, ignore_errors=True)
		self.tftp_dir = None

	def get_output(self):
		# wait a bit to give gem5 time to listen for a telnet connection
		if self.child.poll() != None: # Check if child is down
			print ' '.join(['gem5 is down, return code is ', self.child.returncode])
			return None
		self.telnet = subprocess.Popen(['telnet','localhost','3456'], stdout=subprocess.PIPE)
		return self.telnet.stdout
	
class Gem5MachineARM(Gem5MachineBase):
	def get_bootarch(self):
		return 'armv7'
	
	def set_bootmodules(self, modules):
		# store path to kernel for _get_cmdline to use
		tftp_dir = self.get_tftp_dir()
		self.kernel_img = os.path.join(self.options.buildbase, self.options.builds[0].name, 'arm_gem5_image')
		
		#write menu.lst
		path = os.path.join(self.get_tftp_dir(), 'menu.lst')
		self._write_menu_lst(modules.get_menu_data('/'), path)

# SK: did not test this yet, but should work
# @machines.add_machine
# class Gem5MachineARMSingleCore(Gem5MachineARM):
# 	name = 'gem5_arm_1'
	
# 	def get_ncores(self):
# 		return 1
	
# 	def _get_cmdline(self):
# 		script_path = os.path.join(self.options.sourcedir, 'tools/arm_gem5', 'gem5script.py')
# 		return (['gem5.fast', script_path, '--kernel=%s'%self.kernel_img, '--n=%s'%self.get_ncores()]
# 				+ GEM5_CACHES_ENABLE)

@machines.add_machine
class Gem5MachineARMMultiCore(Gem5MachineARM):
	name = 'armv7_gem5_2'
	
	def get_bootarch(self):
		return "armv7"
	
	def get_ncores(self):
		return 2
		
	def get_cores_per_socket(self):
		return 1
	
	def _get_cmdline(self):
		script_path = os.path.join(self.options.sourcedir, 'tools/arm_gem5', 'gem5script.py')
		return ([GEM5_PATH + 'gem5.fast', script_path, \
				 '--kernel=%s'%self.kernel_img, \
				 '--caches', \
				 '--l2cache', \
				 '--n=%s' % self.get_ncores()]
				+ GEM5_CACHES_ENABLE)		

# SK: this will not work, since gem5 uses the menu.lst specified in the arm_gem5_image 
#    make target. There, only two cores are booted.
# @machines.add_machine
# class Gem5MachineARMMultiCore(Gem5MachineARM):
# 	name = 'gem5_arm_4'
	
# 	def get_ncores(self):
# 		return 4
		
# 	def get_cores_per_socket(self):
# 		return 2
	
# 	def _get_cmdline(self):
# 		script_path = os.path.join(self.options.sourcedir, 'tools/arm_gem5', 'gem5script.py')
# 		return (['gem5.fast', script_path, '--kernel=%s'%self.kernel_img, '--n=%s'%self.get_ncores()]
# 				+ GEM5_CACHES_ENABLE)
	
	
	
	
	
	
	
	
	
	
	

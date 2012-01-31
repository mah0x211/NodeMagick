import Options
import Environment
import sys, os, shutil, glob
from os import unlink, symlink, popen
from os.path import join, dirname, abspath, normpath

srcdir = '.'
blddir = 'build'
VERSION = '0.5.0'

def set_options(opt):
	opt.tool_options('compiler_cxx')
	opt.tool_options('compiler_cc')
	opt.tool_options('misc')
	
	opt.add_option( '--magick-includes'
		, action='store'
		, type='string'
		, default=False
		, help='Directory containing magick header files'
		, dest='magick_includes'
		)
	
	opt.add_option( '--magick'
		, action='store'
		, type='string'
		, default=False
		, help='Link to a shared magick libraries'
		, dest='magick'
		)

def configure(conf):
	conf.check_tool('compiler_cxx')
	if not conf.env.CXX: conf.fatal('c++ compiler not found')
	conf.check_tool('compiler_cc')
	if not conf.env.CC: conf.fatal('c compiler not found')
	conf.check_tool('node_addon')
	
	o = Options.options
	
	if o.magick_includes:
	    conf.env.append_value("CPPFLAGS", '-I%s' % o.magick_includes)
	
	if o.magick:
	    conf.env.append_value("LINKFLAGS", '-L%s' % o.magick)
	
	# print conf.env
	
	# check libs
	conf.check_cc( lib='MagickWand', mandatory=True )

def build(bld):
	# print 'build'
	t = bld.new_task_gen('cxx', 'shlib', 'node_addon')
	t.target = 'NodeMagick'
	t.source = './src/NodeMagick.cc'
	t.includes = ['.']
	t.lib = ['MagickWand']

def shutdown(ctx):
	pass

"""
This script looks at a given .h file, and finds each struct Purple* definition.

That Purple struct becomes a seperate class file in api/.  The script will then search
for any functions that operate on that struct and add them as DllImports.

example:

struct _PurpleBuddy { /* blahblahblah */ }
...
char* purple_buddy_get_alias(PurpleBuddy*);

becomes:

class Buddy : Object {
	
	[DllImport("libpurple")]
	IntPtr purple_buddy_get_alias(IntPtr b);
}

The script will have to handle:
* Translation of C types to C# types (any pointer -> IntPtr)
* Handle subclassing (i.e. Buddy should subclass from BListNode)

TODO: Figure out a clean way to create C# Properties for a given class
"""

import sys
import re
import string

def to_function_name(str):
	out = ""
	for c in str:
		if c in string.ascii_uppercase:
			out += "_" + string.lower(c)
		else:
			out += c
	return re.sub("^_", "", out)

def convertname(name):
	newname = ""
	for w in name.split("_"):
		newname += w.capitalize()
	return newname

def clean_arg(arg):
	return re.sub("const", "", arg.strip()).strip()

def is_purple_object(arg):
	a = clean_arg(arg).split()
	if len(a) != 2:
		return False
	return (re.match("^Purple", a[0]) and a[1].count('*') == 1)

primitive_types = {
	'gboolean': 'bool',
}

def determine_marshall_type(arg):
	"""
	Returns the type used to marshall between C# and C code (i.e. PurpleBuddy* -> IntPtr)
	TODO: handle "primitive" types (i.e. gboolean -> bool)
	"""
	a = clean_arg(arg).split()
	if len(a) > 1:
		if a[1].count('*') == 1:
			if a[0] == 'char' and len(a[1]) > 1:
				return "string"
			return "IntPtr"

	if a[0] in primitive_types:
		return primitive_types[a[0]]
	return a[0]

def determine_csharp_type(arg):
	"""
	Returns the type used by the C# Purple API (i.e. PurpleBuddy* -> Buddy)
	TODO: handle "primitive" types (i.e. gboolean -> bool)
	"""
	a = clean_arg(arg).split()
	if is_purple_object(arg):
		return re.sub("^Purple", "", a[0])
	if re.match("char", a[0]) and a[1].count('*') == 1:
		return "string"

	if a[0] in primitive_types:
		return primitive_types[a[0]]
	return a[0]

class ArgType:
	def __init__(self, cstr):
		self.marshall = determine_marshall_type(cstr)
		self.csharp = determine_csharp_type(cstr)
		self.is_purple_object = is_purple_object(cstr) 

class Property:
	def __init__(self, propname, csclassname):
		self.propname = propname.strip('_')
		self.cspropname = convertname(self.propname) + "Prop"
		self.csclassname = csclassname

		self.actions = {}

	def add(self, operation, method):
		self.actions[operation] = method

	def dump(self):
		property = """
		public %s %s {
			%s
		}
		"""

		body = ""
		ret_type = None
		if self.actions.has_key('get'):
			meth = self.actions['get']
			method_call = meth.func_str + "("
			args = []
			for a in meth.arg_type_list:
				if a.marshall == "IntPtr" and a.csharp == self.csclassname:
					args.append("Handle")

			method_call += string.join(args, ", ") + ")"
			body += "get {\n\t\t\t\t"
			body += "return "
			ret_type = meth.ret_type.csharp
			if meth.ret_type.csharp == "string":
				body += "Util.build_string(%s);" % (method_call)
			elif meth.ret_type.is_purple_object:
				body += "ObjectManager.GetObject(%s, type(%s)) as %s;" % (method_call, ret_type, ret_type)
			else:
				body += method_call + ";"

			body += "\n\t\t\t}\n"

		if self.actions.has_key('set'):
			meth = self.actions['set']
			method_call = meth.func_str + "("
			args = []
			print meth
			for a in meth.arg_type_list:
				print "\t" + a.csharp
				if a.marshall == "IntPtr" and a.csharp == self.csclassname:
					args.append("Handle")
				if a.csharp == ret_type:
					if a.is_purple_object:
						args.append("value.Handle")
					else:
						args.append("value")
			method_call += string.join(args, ", ") + ");"
			body += "\t\t\tset {\n\t\t\t\t"
			body += method_call 
			body += "\n\t\t\t}\n"

		return property % (ret_type, self.cspropname, body)

	def __str__(self):
		return self.dump()


class Method:
	def __init__(self, ret_str, func_str, arg_str_list):
		self.ret_str = ret_str
		self.func_str = func_str
		self.arg_str_list = arg_str_list

		self.ret_type = ArgType(self.ret_str)

		self.arg_type_list = []

		for arg in self.arg_str_list:
			self.arg_type_list.append(ArgType(arg))

	def as_dllimport(self):
		dllimport = """
		[DllImport("libpurple")]
		static private extern %s %s(%s);
		"""

		return dllimport % (self.ret_type.marshall, self.func_str, string.join([s.marshall for s in self.arg_type_list], ","))

	def __repr__(self):
		return self.func_str
		
class Struct:
	def __init__(self, structname):
		self.structname = structname
		self.csname = re.sub("^Purple", "", structname)
		func_name = to_function_name(structname)
		self.func_nonstatic_parse_str = "(const\s+)?([A-Za-z]+\s*\*?)\s*(%s_(get|set|new|on)(.*))\((.*)\);" % (func_name)
		self.func_parse_str = "(const\s+)?([A-Za-z]+\s*\*?)\s*(%s[A-Za-z0-9_]+)\((.*)\);" % (func_name)
		self.func_nonstatic_parse = re.compile(self.func_nonstatic_parse_str)

		self.nonstatic_methods = []

		self.properties = {}

	def is_my_func(self, str):
		match = self.func_nonstatic_parse.match(str)

		if not match:
			return False

		ret_type = match.group(2)
		func_name = match.group(3)
		operation = match.group(4)
		prop_name = match.group(5)
		args = match.group(6)

		meth = Method(ret_type, func_name, args.split(','))

		if operation != "new":
			if not self.properties.has_key(prop_name):
				self.properties[prop_name] = Property(prop_name, self.csname)

			self.properties[prop_name].add(operation, meth)

		self.nonstatic_methods.append(meth)

		return True

	def dump_constructors(self):
		intptr_constructor = """
		public %s(IntPtr handle)
			: base(handle)
		{
		}
		"""

		return intptr_constructor % (self.csname)

	def dump(self):
		out = """
using System;
using System.Runtime.InteropServices;

namespace Purple {
	class %s : Object {
		%s
	}
}
	"""
		body = ""

		body += self.dump_constructors()

		for p in self.properties:
			body += str(self.properties[p])

		for m in self.nonstatic_methods:
			body += m.as_dllimport()

		print out % (self.csname, body)

	def __str__(self):
		return self.structname

def parse_structs_and_funcs():
	parse_struct = re.compile("struct _(Purple[A-Za-z]+).*")

	find_function = re.compile("(const\s+)?[A-Za-z]+\s*\*?\s*[A-Za-z0-9_]+\(.*")
	func_parse = re.compile("(const\s+)?([A-Za-z]+\s*\*?)\s*([A-Za-z0-9_]+)\((.*)\);")

	input = iter(sys.stdin)

	structs = {} 

	for line in input:
		struct_match = parse_struct.match(line)
		func_find = find_function.match(line)
		if struct_match:
			structs[struct_match.group(1)] = Struct(struct_match.group(1))
		elif func_find:
			func_line = line.strip()
			while func_line.count('(') > func_line.count(')'):
        	       		newline = input.next().strip()
	        	        if len(newline) == 0:
        	        	    break
	                	func_line += newline

			for k, s in structs.items():
				if s.is_my_func(func_line):
					break
	
	return structs

structs = parse_structs_and_funcs()

if len(sys.argv) < 2:
	print "I need to know what you want me to make!  pass in one of the structs below as the first arg:"
	for k, s in structs.items():
		print s
else:
	struct_to_build = sys.argv[1]
	structs[struct_to_build].dump()
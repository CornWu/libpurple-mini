import sys
import re
import string



def parse_signal_register_calls(input):
	signal_reg = re.compile(".*purple_signal_register.*")
	end_call = re.compile(".*\);")

	buffer = ""
	parse_signal_reg = False

	signal_regs = []

	for line in input:
		if not parse_signal_reg:
			if signal_reg.match(line):
				buffer = line.strip()
				if end_call.match(line):
					signal_regs.append(buffer)
					parse_signal_reg = False
					continue
				parse_signal_reg = True
				continue

		if parse_signal_reg:
			buffer = buffer + line.strip()
			if end_call.match(line):
				signal_regs.append(buffer)
				parse_signal_reg = False
				continue
	
	return signal_regs


def parse_signal_register_details(signal_regs):
	func_parse = re.compile("purple_signal_register\(handle\s*,\s*\"([A-Za-z0-9_-]+)\"\s*,\s*([A-Za-z0-9_-]+)\s*,\s*(.+)\s*,\s*([0-9]+)\s*,?\s*(.*)\s*(\);)$");

	return_type_parse = re.compile("(purple_value_new\(PURPLE_TYPE_SUBTYPE,|purple_value_new\()?\s*([A-Za-z_-]+)")

	signals = {}

	for func in signal_regs:
		m = func_parse.match(func)
	
		if m:
			signal_name = m.group(1)
			marshall = m.group(2)
			return_type_str = m.group(3)
			number_args = int(m.group(4))

			return_type_m = return_type_parse.match(return_type_str)

			signals[signal_name] = [return_type_m.group(2)]

			#print "Parsing signal: " + signal_name

			if number_args > 0:
				values = m.group(5)
	
				value_parse = re.compile("purple_value_new\((PURPLE_TYPE_SUBTYPE|PURPLE_TYPE_BOXED)?,?\s*(.+)\)\s*,?\s*" * number_args)

				v = value_parse.match(values)

				if v:
					for g in v.groups():
						if g and g != "PURPLE_TYPE_SUBTYPE" and g != "PURPLE_TYPE_BOXED":
							signals[signal_name].append(g)

	return signals


	
input = iter(sys.stdin)

for k, v in parse_signal_register_details(parse_signal_register_calls(input)).items():
	print k, v

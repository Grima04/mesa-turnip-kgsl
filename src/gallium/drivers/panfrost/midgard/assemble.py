"""
Copyright (C) 2018 Alyssa Rosenzweig
Copyright (c) 2013 Connor Abbott (connor@abbott.cx)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
"""

import sys
import pprint
import struct

program = []

# Definitions from cwabbott's tools

t6xx_alu_ops = {
    "fadd":  0x10,
    "fmul":  0x14,
    "fmin":  0x28,
    "fmax":  0x2C,
    "fmov":  0x30,
    "ffloor":  0x36,
    "fceil":  0x37,
    "fdot3":  0x3C,
    "fdot3r":  0x3D,
    "fdot4":  0x3E,
    "freduce":  0x3F,
    "iadd":  0x40,
    "isub":  0x46,
    "imul":  0x58,
    "imov":  0x7B,
    "feq":  0x80,
    "fne":  0x81,
    "flt":  0x82,
    "fle":  0x83,
    "f2i":  0x99,
    "f2u8":  0x9C,
    "u2f": 0xBC,
    "ieq":  0xA0,
    "ine":  0xA1,
    "ilt":  0xA4,
    "ile":  0xA5,
    "iand": 0x70,
    "ior": 0x71,
    "inot": 0x72,
    "iandnot": 0x74,
    "ixor": 0x76,
    "ball":  0xA9,
    "bany":  0xB1,
    "i2f":  0xB8,
    "csel":  0xC5,
    "fatan_pt2":  0xE8,
    "frcp":  0xF0,
    "frsqrt":  0xF2,
    "fsqrt":  0xF3,
    "fexp2":  0xF4,
    "flog2":  0xF5,
    "fsin":  0xF6,
    "fcos":  0xF7,
    "fatan2_pt1":  0xF9,
}

t6xx_alu_bits = {
        "vmul": 17,
        "sadd": 19,
        "vadd": 21,
        "smul": 23,
        "lut": 25,
        "br": 26,
        "branch": 27,
        "constants": 32
}

t6xx_alu_size_bits = {
        "vmul": 48,
        "sadd": 32,
        "vadd": 48,
        "smul": 32,
        "lut": 48,
        "br": 16,
        "branch": 48
}

t6xx_outmod = {
        "none": 0,
        "pos": 1,
        "int": 2,
        "sat": 3
}

t6xx_reg_mode = {
    "quarter": 0,
    "half": 1,
    "full": 2,
    "double": 3
}

t6xx_dest_override = {
    "lower": 0,
    "upper": 1,
    "none": 2
}

t6xx_load_store_ops = {
    "ld_st_noop":  0x03,
    "ld_attr_16":  0x95,
    "ld_attr_32":  0x94,
    "ld_vary_16":  0x99,
    "ld_vary_32":  0x98,
    "ld_uniform_16":  0xAC,
    "ld_uniform_32":  0xB0,
    "st_vary_16":  0xD5,
    "st_vary_32":  0xD4,
    "ld_color_buffer_8": 0xBA
}

t6xx_tag = {
        "texture": 0x3,
        "load_store": 0x5,
        "alu4": 0x8,
        "alu8": 0x9,
        "alu12": 0xA,
        "alu16": 0xB,
}

def is_tag_alu(tag):
    return (tag >= t6xx_tag["alu4"]) and (tag <= t6xx_tag["alu16"])

# Just an enum

ALU = 0
LDST = 1
TEXTURE = 2

# Constant types supported, mapping the constant prefix to the Python format
# string and the coercion function

constant_types = {
        "f": ("f", float),
        "h": ("e", float),
        "i": ("i", int),
        "s": ("h", int)
}

compact_branch_op = {
        "jump": 1,
        "branch": 2,
        "discard": 4,
        "write": 7
}

branch_condition = {
        "false": 1,
        "true": 2,
        "always": 3,
}

# TODO: What else?

texture_op = {
        "normal": 0x11,
        "texelfetch": 0x14
}

texture_fmt = {
        "2d": 0x02,
        "3d": 0x03
}
	
with open(sys.argv[1], "r") as f:
    for ln in f:
        space = ln.strip().split(" ")

        instruction = space[0]
        rest = " ".join(space[1:])

        arguments = [s.strip() for s in rest.split(",")]
        program += [(instruction, arguments)]

swizzle_component = {
        "x": 0,
        "y": 1,
        "z": 2,
        "w": 3
}

def decode_reg_name(reg_name):
    ireg = 0
    upper = False
    half = False

    if reg_name[0] == 'r':
        ireg = int(reg_name[1:])
    elif reg_name[0] == 'h':
        rreg = int(reg_name[2:])

        # Decode half-register into its full register's half
        ireg = rreg >> 1
        upper = rreg & 1
        half = True
    else:
        # Special case for load/store addresses
        ireg = int(reg_name)

    return (ireg, half, upper)

def standard_swizzle_from_parts(swizzle_parts):
    swizzle_s = swizzle_parts[1] if len(swizzle_parts) > 1 else "xyzw"

    swizzle = 0
    for (i, c) in enumerate(swizzle_s):
        swizzle |= swizzle_component[c] << (2 * i)

    return swizzle

def mask_from_parts(mask_parts, large_mask):
    mask_s = mask_parts[1] if len(mask_parts) > 1 else "xyzw"

    if large_mask:
        mask = sum([(3 << (2*swizzle_component[c]) if c in mask_s else 0) for c in "xyzw"])
    else:
        mask = sum([(1 << swizzle_component[c] if c in mask_s else 0) for c in "xyzw"])

    return (mask, mask_s)

def decode_reg(reg):
    if reg[0] == "#":
        # Not actually a register, instead an immediate float
        return (True, struct.unpack("H", struct.pack("e", float(reg[1:])))[0], 0, 0, 0, 0)

    # Function call syntax used in abs() modifier
    if reg[-1] == ')':
        reg = reg[:-1]

    swizzle_parts = reg.split(".")

    reg_name = swizzle_parts[0]

    modifiers = 0

    if reg_name[0] == '-':
        modifiers |= 2
        reg_name = reg_name[1:]

    if reg_name[0] == 'a':
        modifiers |= 1
        reg_name = reg_name[len("abs("):]
    
    (ireg, half, upper) = decode_reg_name(reg_name)

    return (False, ireg, standard_swizzle_from_parts(swizzle_parts), half, upper, modifiers)

def decode_masked_reg(reg, large_mask):
    mask_parts = reg.split(".")

    reg_name = mask_parts[0]
    (ireg, half, upper) = decode_reg_name(reg_name)
    (mask, mask_s) = mask_from_parts(mask_parts, large_mask)

    component = max([0] + [swizzle_component[c] for c in "xyzw" if c in mask_s])

    return (ireg, mask, component, half, upper)

# TODO: Fill these in XXX

# Texture pipeline registers in r28-r29
TEXTURE_BASE = 28

def decode_texture_reg_number(reg):
    r = reg.split(".")[0]

    if r[0] == "r":
        return (True, int(r[1:]) - TEXTURE_BASE, 0)
    else:
        no = int(r[2:])
        return (False, (no >> 1) - TEXTURE_BASE, no & 1)

def decode_texture_reg(reg):
    (full, select, upper) = decode_texture_reg_number(reg)

    # Swizzle mandatory for texture registers, afaict
    swizzle = reg.split(".")[1]
    swizzleL = swizzle_component[swizzle[0]]
    swizzleR = swizzle_component[swizzle[1]]

    return (full, select, upper, swizzleR, swizzleL)

def decode_texture_out_reg(reg):
    (full, select, upper) = decode_texture_reg_number(reg)
    (mask, _) = mask_from_parts(reg.split("."), False)

    return (full, select, upper, mask)

instruction_stream = []

for p in program:
    ins = p[0]
    arguments = p[1]

    family = ins_mod = ins.split(".")[0]
    ins_op = (ins + ".").split(".")[1]

    ins_outmod = (ins + "." + ".").split(".")[2]
    
    try:
        out_mod = t6xx_outmod[ins_outmod]
    except:
        out_mod = 0

    if ins in t6xx_load_store_ops:
        op = t6xx_load_store_ops[ins]
        (reg, mask, component, half, upper) = decode_masked_reg(p[1][0], False)
        (immediate, address, swizzle, half, upper, modifiers) = decode_reg(p[1][1])
        unknown = int(p[1][2], 16)
        b = (op << 0) | (reg << 8) | (mask << 13) | (swizzle << 17) | (unknown << 25) | (address << 51)
        instruction_stream += [(LDST, b)]
    elif ins_op in t6xx_alu_ops:
        op = t6xx_alu_ops[ins_op]

        (reg_out, mask, out_component, half0, upper0) = decode_masked_reg(p[1][0], True)
        (_, reg_in1, swizzle1, half1, upper1, mod1) = decode_reg(p[1][1])
        (immediate, reg_in2, swizzle2, half2, upper2, mod2) = decode_reg(p[1][2])

        if immediate:
            register_word = (reg_in1 << 0) | ((reg_in2 >> 11) << 5) | (reg_out << 10) | (1 << 15)
        else:
            register_word = (reg_in1 << 0) | (reg_in2 << 5) | (reg_out << 10) 

        if ins_mod in ["vadd", "vmul", "lut"]:
            io_mode = t6xx_reg_mode["half" if half0 else "full"]
            repsel = 0
            i1half = half1
            i2block = 0
            output_override = 2 # NORMAL, TODO
            wr_mask = 0

            if (ins_outmod == "quarter"):
                io_mode = t6xx_reg_mode["quarter"]

            if half0:
                # TODO: half actually
                repsel = 2 * upper1 
            else:
                repsel = upper1

            if half0:
                # Rare case...

                (_, halfmask, _, _, _) = decode_masked_reg(p[1][0], False)
                wr_mask = halfmask
            else:
                wr_mask = mask


            if immediate:
                # Inline constant: lower 11 bits

                i2block = ((reg_in2 & 0xFF) << 3) | ((reg_in2 >> 8) & 0x7)
            else:
                if half0:
                    # TODO: replicate input 2 if half
                    pass
                else:
                    # TODO: half selection
                    i2block = upper2 | (half2 << 2)

                i2block |= swizzle2 << 3

            # Extra modifier for some special cased stuff
            try:
                special = ins.split(".")[3]

                if special == "low":
                    output_override = 0 # low
                elif special == "fulllow":
                    # TODO: Not really a special case, just a bug?
                    io_mode = t6xx_reg_mode["full"]
                    output_override = 0 #low
                    wr_mask = 0xFF
            except:
                pass

            instruction_word = (op << 0) | (io_mode << 8) | (mod1 << 10) | (repsel << 12) | (i1half << 14) | (swizzle1 << 15) | (mod2 << 23) | (i2block << 25) | (output_override << 36) | (out_mod << 38) | (wr_mask << 40)
        elif ins_mod in ["sadd", "smul"]:
            # TODO: What are these?
            unknown2 = 0
            unknown3 = 0

            i1comp_block = 0

            if half1:
                i1comp_block = swizzle1 | (upper1 << 2)
            else:
                i1comp_block = swizzle1 << 1

            i2block = 0

            if immediate:
                # Inline constant is splattered in a... bizarre way

                i2block = (((reg_in2 >> 9) & 3) << 0) | (((reg_in2 >> 8) & 1) << 2) | (((reg_in2 >> 5) & 7) << 3) | (((reg_in2 >> 0) & 15) << 6)
            else:
                # TODO: half register
                swizzle2 = (swizzle2 << 1) & 0x1F
                i2block = (mod2 << 0) | ((not half2) << 2) | (swizzle2 << 3) | (unknown2 << 5)

            outcomp_block = 0
            
            if True:
                outcomp_block = out_component << 1
            else:
                # TODO: half register
                pass

            instruction_word = (op << 0) | (mod1 << 8) | ((not half1) << 10) | (i1comp_block << 11) | (i2block << 14) | (unknown3 << 25) | (out_mod << 26) | ((not half0) << 28) | (outcomp_block) << 29

        else:
            instruction_word = op

        instruction_stream += [(ALU, ins_mod, register_word, instruction_word)]
    elif family == "texture":
        # Texture ops use long series of modifiers to describe their needed
        # capabilities, seperated by dots. Decode them here
        parts = ins.split(".")

        # First few modifiers are fixed, like an instruction name
        tex_op = parts[1]
        tex_fmt = parts[2]

        # The remaining are variable, but strictly ordered
        parts = parts[3:]

        op = texture_op[tex_op]

        # Some bits are defined directly in the modifier list
        shadow = "shadow" in parts
        cont = "cont" in parts
        last = "last" in parts
        has_filter = "raw" not in parts

        # The remaining need order preserved since they have their own arguments
        argument_parts = [part for part in parts if part not in ["shadow", "cont", "last", "raw"]]

        bias_lod = 0

        for argument, part in zip(argument_parts, arguments[4:]):
            if argument == "bias":
                bias_lod = int(float(part) * 256)
            else:
                print("Unknown argument: " + str(argument))

        fmt = texture_fmt[tex_fmt]
        has_offset = 0

        magic1 = 1 # IDEK
        magic2 = 2 # Where did this even come from?!

        texture_handle = int(arguments[1][len("texture"):])
        
        sampler_parts = arguments[2].split(".")
        sampler_handle = int(sampler_parts[0][len("sampler"):])
        swizzle0 = standard_swizzle_from_parts(sampler_parts)

        (full0, select0, upper0, mask0) = decode_texture_out_reg(arguments[0])
        (full1, select1, upper1, swizzleR1, swizzleL1) = decode_texture_reg(arguments[3])

        tex = (op << 0) | (shadow << 6) | (cont << 8) | (last << 9) | (fmt << 10) | (has_offset << 15) | (has_filter << 16) | (select1 << 17) | (upper1 << 18) | (swizzleL1 << 19) | (swizzleR1 << 21) | (0 << 23) | (magic2 << 25) | (full0 << 29) | (magic1 << 30) | (select0 << 32) | (upper0 << 33) | (mask0 << 34) | (swizzle0 << 40) | (bias_lod << 72) | (texture_handle << 88) | (sampler_handle << 104)

        instruction_stream += [(TEXTURE, tex)]
    elif family == "br":
        cond = ins.split(".")[2]
        condition = branch_condition[cond]
        bop = compact_branch_op[ins_op]

        offset = int(arguments[0].split("->")[0])

        # 2's complement and chill
        if offset < 0:
            offset = (1 << 7) - abs(offset)

        # Find where we're going
        dest_tag = int(arguments[0].split("->")[1])

        br = (bop << 0) | (dest_tag << 3) | (offset << 7) | (condition << 14)

        # TODO: Unconditional branch encoding

        instruction_stream += [(ALU, "br", None, br)]
    elif ins[1:] == "constants":
        if ins[0] not in constant_types:
            print("Unknown constant type " + str(constant_type))
            break

        (fmt, cast) = constant_types[ins[0]]

        encoded = [struct.pack(fmt, cast(f)) for f in p[1]]

        consts = bytearray()
        for c in encoded:
            consts += c

        # consts must be exactly 4 quadwords, so pad with zeroes if necessary
        consts += bytes(4*4 - len(consts))

        instruction_stream += [(ALU, "constants", consts)]

# Emit from instruction stream
instructions = []
index = 0
while index < len(instruction_stream):
    output_stream = bytearray()
    ins = instruction_stream[index]
    tag = ins[0]

    can_prefetch = index + 1 < len(instruction_stream)
    succeeding = None

    if tag == LDST:
        succeeding = instruction_stream[index + 1] if can_prefetch else None
        parta = ins[1]
        partb = None

        if succeeding and succeeding[0] == LDST:
            partb = succeeding[1]
            index += 1
        else:
            partb = parta
            parta = t6xx_load_store_ops["ld_st_noop"]

        tag8 = t6xx_tag["load_store"]

        ins = (partb << 68) | (parta << 8) | tag8
        output_stream += (ins.to_bytes(16, "little"))
    elif tag == TEXTURE:
        tag8 = t6xx_tag["texture"] 
        ins = (ins[1] << 8) | tag8

        output_stream += (ins.to_bytes(16, "little"))
    elif tag == ALU:
        # TODO: Combining ALU ops

        emit_size = 4 # 32-bit tag always emitted

        tag = 0
        register_words = bytearray()
        body_words = bytearray()
        constant_words = None

        last_alu_bit = 0

        # Iterate through while there are ALU tags in strictly ascending order
        while index < len(instruction_stream) and instruction_stream[index][0] == ALU and t6xx_alu_bits[instruction_stream[index][1]] > last_alu_bit:
            ins = instruction_stream[index]

            bit = t6xx_alu_bits[ins[1]]
            last_alu_bit = bit

            if ins[1] == "constants":
                constant_words = ins[2]
            else:
                # Flag for the used part of the GPU
                tag |= 1 << bit

                # 16-bit register word, if present
                if ins[2] is not None:
                    register_words += (ins[2].to_bytes(2, "little"))
                    emit_size += 2

                size = int(t6xx_alu_size_bits[ins[1]] / 8)
                body_words += (ins[3].to_bytes(size, "little"))
                emit_size += size

            index += 1

        index -= 1 # fix off by one, from later loop increment

        # Pad to nearest multiple of 4 words
        padding = (16 - (emit_size & 15)) if (emit_size & 15) else 0
        emit_size += padding

        # emit_size includes constants
        if constant_words:
            emit_size += len(constant_words)

        # Calculate tag given size
        words = emit_size >> 2
        tag |= t6xx_tag["alu" + str(words)]

        # Actually emit, now that we can
        output_stream += tag.to_bytes(4, "little")
        output_stream += register_words
        output_stream += body_words
        output_stream += bytes(padding)

        if constant_words:
            output_stream += constant_words

    instructions += [output_stream]
    index += 1

# Assmebly over; just emit tags at this point
binary = bytearray()

for (idx, ins) in enumerate(instructions):
    # Instruction prefetch
    tag = 0

    if idx + 1 < len(instructions):
        tag = instructions[idx + 1][0] & 0xF

        # Check for ALU special case

        if is_tag_alu(tag) and idx + 2 == len(instructions):
            tag = 1
    else:
        # Instruction stream over
        
        tag = 1

    ins[0] |= tag << 4

    binary += ins

pprint.pprint(program)

with open(sys.argv[2], "wb") as f:
    f.write(binary)

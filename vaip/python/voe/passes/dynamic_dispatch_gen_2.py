##
##  Copyright (C) 2023 – 2024 Advanced Micro Devices, Inc. All rights reserved.
##  Licensed under the MIT License.
##
import sys
import os
import string

# import glog as log
import numpy as np
from voe.anchor_point import CONST
from voe.pattern import node, wildcard, xir_const_op
from voe.rule_ext import Rule, same_as
import builtins

from pathlib import PurePath
from .fuse import generate_transactions as TxnGenerator

verbose = False


class dynamic_dispatcher_generator_2(Rule):
    def __init__(self):
        self.matched = False

    def pattern(self):
        if verbose:
            print("-- DynamicDispatchGeneator::pattern pass::start")
        dummy = node("DequantizeLinear", wildcard(), wildcard(), wildcard())
        return dummy.build(locals())

    def where(self, dummy, **kwargs):
        if self.matched:
            return False
        else:
            self.matched = True
            return True

    def action(self, dummy, **kwargs):
        if verbose:
            print("-- DynamicDispatchGeneator::action pass::start")
        model_path = PurePath(self.cache_dir(), "fused.onnx")
        if verbose:
            print("-- CacheDir: {}".format(self.cache_dir()))
            print("-- Model Path: {}".format(model_path))
        input_names, output_names = TxnGenerator(model_path, verbose=verbose)
        if verbose:
            print("-- DynamicDispatchGeneator::action pass::done")

        if verbose:
            print("-- Input Names: ", input_names)
            print("-- Output Names: ", output_names)

        # meta_def = self.try_fuse_with_name("DOD_"+input_names[0].replace("/", "_"), input_names, output_names, [], "DOD")
        # print("-- Try fuse with name: {}".format(meta_def))
        # Always return false
        # return meta_def.fuse()
        return False


def rules():
    return [dynamic_dispatcher_generator_2()]

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""llm.cpp 绑定路径探针：复刻 LoadLibrary→backend_init→默认参数→模型加载，
定位 LingxiUI 里 LLM 预热线程卡住的位置。"""
import ctypes, os, sys, time

ROOT = r"D:\Visual Studio Code\Something\LingxiIme"
BIN = os.path.join(ROOT, "bin", "Debug", "llama")
MODEL = os.path.join(ROOT, "bin", "Debug", "models", "qwen2.5-0.5b-instruct-q4_k_m.gguf")

os.add_dll_directory(BIN)
t0 = time.time()
def log(m): print(f"[{time.time()-t0:6.1f}s] {m}", flush=True)

log("CDLL llama.dll ...")
dll = ctypes.CDLL(os.path.join(BIN, "llama.dll"))
log("loaded")

dll.llama_backend_init()
log("backend_init ok")

# 新版 llama.cpp 需显式加载后端（CPU 特性分发 DLL 由运行时选择）
ggml = ctypes.CDLL("ggml.dll")
ggml.ggml_backend_load_all()
log("ggml_backend_load_all ok")

# llama_model_params (b11900)：devices*, tensor_buft_overrides*,
# n_gpu_layers i32, split_mode i32, load_mode i32, lazy_mode i32,
# main_gpu i32, tensor_split f32*, progress_callback, cb_user_data,
# kv_overrides*, vocab_overrides*
class llama_model_params(ctypes.Structure):
    _fields_ = [("devices", ctypes.c_void_p),
                ("tensor_buft_overrides", ctypes.c_void_p),
                ("n_gpu_layers", ctypes.c_int32),
                ("split_mode", ctypes.c_int32),
                ("load_mode", ctypes.c_int32),
                ("lazy_mode", ctypes.c_int32),
                ("main_gpu", ctypes.c_int32),
                ("tensor_split", ctypes.c_void_p),
                ("progress_callback", ctypes.c_void_p),
                ("progress_callback_user_data", ctypes.c_void_p),
                ("kv_overrides", ctypes.c_void_p),
                ("vocab_overrides", ctypes.c_void_p)]

dll.llama_model_default_params.restype = llama_model_params
mp = dll.llama_model_default_params()
log(f"default params: n_gpu_layers={mp.n_gpu_layers}")
mp.n_gpu_layers = 0

dll.llama_model_load_from_file.restype = ctypes.c_void_p
dll.llama_model_load_from_file.argtypes = [ctypes.c_char_p, llama_model_params]
log("loading model ...")
model = dll.llama_model_load_from_file(MODEL.encode(), mp)
log(f"model handle = {model:#x}" if model else "MODEL LOAD FAILED")
if not model:
    sys.exit(1)

dll.llama_model_free(model)
dll.llama_backend_free()
log("probe complete OK")

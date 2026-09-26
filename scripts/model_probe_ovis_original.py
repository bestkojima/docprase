#!/usr/bin/env python3
"""用官方 Transformers 接口运行本地原始 OvisOCR2 权重作转换基线。"""
import argparse
import hashlib
import json
from pathlib import Path
import time

from PIL import Image
import torch
from transformers import AutoModelForMultimodalLM, AutoProcessor

from model_probe_ovis import PROMPT


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=Path("/home/dr/project/models/ovrics_ocrv2"))
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--max-tokens", type=int, default=512)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    processor = AutoProcessor.from_pretrained(args.model, local_files_only=True)
    model = AutoModelForMultimodalLM.from_pretrained(
        args.model, local_files_only=True, dtype=torch.float32,
    )
    model.eval()
    messages = [{"role": "user", "content": [{"type": "image", "image": Image.open(args.image).convert("RGB")},
                                             {"type": "text", "text": PROMPT}]}]
    inputs = processor.apply_chat_template(messages, add_generation_prompt=True,
                                           tokenize=True, return_dict=True,
                                           return_tensors="pt", enable_thinking=False)
    # The bundled text_config uses <|endoftext|> as EOS, while the chat template
    # closes assistant turns with <|im_end|>. The author vLLM example stops on
    # the latter. Without this override Transformers repeats until the cap.
    chat_eos = processor.tokenizer.convert_tokens_to_ids("<|im_end|>")
    with torch.inference_mode():
        outputs = model.generate(**inputs, max_new_tokens=args.max_tokens, do_sample=False,
                                 eos_token_id=chat_eos,
                                 pad_token_id=processor.tokenizer.pad_token_id,
                                 return_dict_in_generate=True)
    tokens = outputs.sequences[0][inputs["input_ids"].shape[-1]:]
    raw = processor.decode(tokens, skip_special_tokens=True)
    (args.out / "raw.txt").write_text(raw)
    eos_ids = [chat_eos]
    stop_reason = ("normal" if len(tokens) and tokens[-1].item() in eos_ids
                   else "token_limit" if len(tokens) >= args.max_tokens else "unknown")
    report = {"source": "ATH-MaaS/OvisOCR2 Transformers original framework",
              "model_path": str(args.model.resolve()),
              "model_weight_sha256": sha256(args.model / "model.safetensors"),
              "image_sha256": sha256(args.image), "prompt": PROMPT,
              "max_new_tokens": args.max_tokens, "do_sample": False,
              "generated_tokens": len(tokens), "stop_reason": stop_reason,
              "last_token_id": tokens[-1].item() if len(tokens) else None,
              "eos_token_ids": eos_ids, "elapsed_seconds": round(time.monotonic() - started, 3),
              "torch": torch.__version__, "transformers": __import__("transformers").__version__,
              "cpu": True, "dtype": "float32", "raw_sha256": sha256(args.out / "raw.txt")}
    (args.out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()

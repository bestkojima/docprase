"""#31：冻结父区域评测版本并串行新跑20页、旧7页和12页全集。"""
from issue27_run import main
from issue31_suite import freeze_evaluation


if __name__ == '__main__':
    raise SystemExit(main(freeze_evaluation_fn=freeze_evaluation))

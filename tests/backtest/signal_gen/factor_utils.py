"""因子计算相关辅助操作
"""

from itertools import islice


def get_nth_last_element(dictionary, n, get_value=False):
    """
    高效获取倒数第n个元素
    :param get_value: True返回值，False返回键
    """
    if get_value:
        it = reversed(dictionary.values())
    else:
        it = reversed(dictionary)
    
    return next(islice(it, n-1, None))
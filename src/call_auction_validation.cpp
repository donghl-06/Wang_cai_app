#include <iostream>
#include "OrderLoader.h"

using namespace std;

int main() {
    cout << "=== 集合竞价验证测试程序 ===" << endl;
    
    // 可以验证多个股票的集合竞价
    vector<pair<string, string>> test_cases = {
        {"000027.SZ", "2022-01-07"},
        {"000028.SZ", "2022-01-05"},
        {"000039.SZ", "2022-01-07"},
        // 可以添加更多测试用例
    };
    
    for (const auto& test_case : test_cases) {
        cout << "\n" << string(50, '=') << endl;
        try {
            validateCallAuction(test_case.first, test_case.second);
        } catch (const exception& e) {
            cout << "验证过程中发生错误: " << e.what() << endl;
        }
        cout << string(50, '=') << endl;
    }
    
    cout << "\n所有验证完成！" << endl;
    return 0;
} 
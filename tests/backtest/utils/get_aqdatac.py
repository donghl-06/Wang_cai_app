import aqdatac
aqdatac.login(username='laimingshu',password='laimingshu')

print(aqdatac.accessible_tables())
df = aqdatac.get_data(table_name='cstick',start_date='2024-01-04',end_date='2024-01-05',sym_list='600276.SH')

df.to_csv('cstick_example.csv')

df = aqdatac.get_data(table_name='csord',start_date='2024-01-04',end_date='2024-01-05',sym_list='600276.SH')

df.to_csv('csord_example.csv')
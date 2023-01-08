import pandas as pd
import matplotlib.pyplot as plt
import glob
import sys

pd.set_option('display.max_columns', None)
pd.set_option('expand_frame_repr', False)

def mapping_memory_samples_to_chunks(df_dram, df_promotion):
    print(df_dram.head())
    print(df_promotion.head())
    sys.exit()
    
    list_mmap_index = []
    list_mmap_call_stack_id = []
    list_mmap_object_name = []

    #Here we are creating an lists of dictionary
    data = {k:[] for k in df_perfmem.columns}

    total_rows_mapped=0
    for index, row in df_perfmem.iterrows():
       df_mmap_temp = datasets.df_mmap.copy()

       mask = (df_mmap_temp['ts_event_start'] <= row['ts_event']) & (row['ts_event'] <= df_mmap_temp['ts_event_end'])
       df_mmap_temp = df_mmap_temp.loc[mask]

       if not df_mmap_temp.empty:
           mask = (df_mmap_temp['start_addr_decimal'] <= row['virt_addr_decimal']) & (row['virt_addr_decimal'] <= df_mmap_temp['end_addr_decimal'])
           df_mmap_temp = df_mmap_temp.loc[mask]

           if not df_mmap_temp.empty:
               total_rows_mapped+= df_mmap_temp.shape[0]
               mmaps_index_match = df_mmap_temp.index.values  #index on dataframe for those mmap that satisfied time and address range
               for row_index_mmap in mmaps_index_match:
                    #if you increase or decrease number of columns during perf-mem you must update here
                    data['ts_event'].append(row['ts_event'])
                    data['virt_addr'].append(row['virt_addr'])
                    data['virt_addr_decimal'].append(row['virt_addr_decimal'])
                    data['page_number'].append(row['page_number'])
                    data['mem_level'].append(row['mem_level'])
                    data['access_weight'].append(row['access_weight'])
                    data['thread_rank'].append(row['thread_rank'])
                    data['tlb'].append(row['tlb'])
                    data['access_type'].append(row['access_type'])

                    list_mmap_index.append(row_index_mmap)
                    list_mmap_call_stack_id.append(datasets.df_mmap.iloc[row_index_mmap]['call_stack_hash'])
                    list_mmap_object_name.append(datasets.df_mmap.iloc[row_index_mmap]['obj_name'])

    #Here we create an dictionary of dataframes
    df = {}
    for col in df_perfmem.columns:
       df[col] = pd.DataFrame(data[col], columns=[col])

    #concat all dataframes
    df_1 = pd.concat(df,join='inner', axis=1,ignore_index=True)
    df_1.columns = df_perfmem.columns

    df_mmap_index = pd.DataFrame(list_mmap_index, columns = ["mmap_index"])
    df_mmap_callstack = pd.DataFrame(list_mmap_call_stack_id, columns = ["call_stack_hash"])
    df_mmap_object = pd.DataFrame(list_mmap_object_name, columns = ["obj_name"])
    df_2 = pd.concat([df_mmap_index,df_mmap_callstack,df_mmap_object],join='inner', axis=1,ignore_index=True)
    df_2.columns=['mmap_index','call_stack_hash','obj_name']

    df_perfmem_with_mmap = pd.concat([df_1,df_2], axis=1)

    return df_perfmem_with_mmap

def plot_migration_info():
    file_path = 'migration_summary.txt'
    sys.stdout = open(file_path, "w")

    files = glob.glob('track_info_*.csv')
    name = files[0].split('.')[0]
    app_dataset = name.split('_')[-2] + "_" + name.split('_')[-1]

    #dataframe of samples
    df_external_access = pd.read_csv("loads.txt", names=["ts_event", "virt_addr", "mem_access"])
    #df_external_access['ts_event'] = df_external_access['ts_event'].astype(int)
    
    df_dram = df_external_access.loc[df_external_access.mem_access == "DRAM_hit"]
    df_dram['virt_addr_decimal'] = df_dram['virt_addr'].apply(int, base=16)
 
    df_nvm = df_external_access.loc[df_external_access.mem_access == "NVM_hit"]
    df_nvm['virt_addr_decimal'] = df_nvm['virt_addr'].apply(int, base=16)
    #----------------------------------
    #dataframe of migration cost
    df_migration = pd.read_csv("preload_migration_cost.txt", sep = ",")
    df_migration.columns = df_migration.columns.str.replace(' ', '')
    
    df_migration['start_addr_decimal'] = df_migration['start_addr'].apply(int, base=16)
    df_migration['end_addr_decimal'] = df_migration['start_addr_decimal'] + df_migration['size']
    
    df_init_data = df_migration.loc[df_migration.bind_type == 1]
    print("Initial Dataplacement")
    print("---------------------")
    print("Amount:", df_init_data.shape[0])
    print("Total Cost (ms):", round(df_init_data.migration_cost_ms.sum(),2))
    print("\nTop 10 expensive")
    print(df_init_data[["migration_cost_ms","size","status_pages_before","nodemask_target_node"]].sort_values(by="migration_cost_ms", ascending=False).head(10))
    #df_init_data[["migration_cost_ms","size","status_pages_before","status_pages_after"]].sort_values(by="migration_cost_ms", ascending=False).to_csv("init_data_plac.csv")

    print("\n\n\nPromotion")
    print("---------------------")
    df_promotion = df_migration.loc[df_migration.bind_type == 2]
    print("Amount:", df_promotion.shape[0])
    print("Total Cost (ms):", round(df_promotion.migration_cost_ms.sum(),2))
    print("\nTop 10 expensive")
    print(df_promotion[["migration_cost_ms","size","status_pages_before","nodemask_target_node"]].sort_values(by="migration_cost_ms", ascending=False).head(10))
    #df_promotion[["migration_cost_ms","size","status_pages_before","status_pages_after"]].sort_values(by="migration_cost_ms", ascending=False).to_csv("promotion.csv")

    #mapping_memory_samples_to_chunks(df_dram, df_promotion)
plot_migration_info()

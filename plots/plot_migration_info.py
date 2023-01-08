import pandas as pd
import matplotlib.pyplot as plt
import glob
import sys

pd.set_option('display.max_columns', None)
pd.set_option('expand_frame_repr', False)

def mapping_memory_samples_to_chunks(df_dram, df_promotion):    
    number_of_access = []
    for index, row in df_promotion.iterrows():
        total_access = 0
        df_dram_temp = df_dram.copy()

 	    #filter the dram access after promotion	
        mask = (df_dram_temp['ts_event'] >= row['timestamp'])
        df_dram_temp = df_dram_temp.loc[mask]

        if not df_dram_temp.empty:
            mask = (row['start_addr_decimal'] <= df_dram_temp['virt_addr_decimal']) & (df_dram_temp['virt_addr_decimal'] <= row['end_addr_decimal'])
            df_dram_temp = df_dram_temp.loc[mask]

            if not df_dram_temp.empty:
                total_access = df_dram_temp.shape[0]

        number_of_access.append(total_access)
        
    df_promotion["num_access_after_migration"] =  number_of_access   

               
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
    
    #remove index  from printing
    blankIndex=[''] * len(df_init_data)
    df_init_data.index=blankIndex
    print(df_init_data.sort_values(by="migration_cost_ms", ascending=False).head(10))
    #df_init_data[["migration_cost_ms","size","status_pages_before","status_pages_after"]].sort_values(by="migration_cost_ms", ascending=False).to_csv("init_data_plac.csv")

    print("\n\n\nPromotion")
    print("---------------------")
    df_promotion = df_migration.loc[df_migration.bind_type == 2]
    print("Amount:", df_promotion.shape[0])
    print("Total Cost (ms):", round(df_promotion.migration_cost_ms.sum(),2))
    print("\nTop 10 expensive")
    
    #remove index  from printing
    blankIndex=[''] * len(df_promotion)
    df_promotion.index=blankIndex

    print(df_promotion.sort_values(by="migration_cost_ms", ascending=False).head(10))
    #df_promotion[["migration_cost_ms","size","status_pages_before","status_pages_after"]].sort_values(by="migration_cost_ms", ascending=False).to_csv("promotion.csv")

    mapping_memory_samples_to_chunks(df_dram, df_promotion)
    df_promotion.sort_values(by="num_access_after_migration", ascending=False).to_csv("promotion_effective.csv")
plot_migration_info()

# W2F: AUTO BATTLER DESIGN DOCUMENT (v2.1)

## 1. UNIVERSE & LORE
- **THE SEED:** The main source of all existence. The entire universe was created from this single seed. It is the source of life and all subsequent universes.
- **OMNILIUM (First Matter):** The very first matter created by The Seed, forming the First Universe.

## 2. SYNERGIES & TRAITS

### ORIGINS (Universes)
- **HELIOS (Sun):** Alesk
- **PHAISA (Void):** Baira
- **HEXAGON:** Cyla, Faire
- **OMNILIUM (Life):** Les, Lum
- **COREGONS (Lost Life):** Soul
- **SELINI (Moon):** Lunis, Myna
- **NAJMI (Stars):** Astra, Vega

### COREGONS (3/6/8) - "The Lost Life"
Coregons units heal for a flat percentage of the damage they deal. As the synergy grows, they summon lost souls and drag the arena into the underworld.
- **(3) The Ghouls:** Heal for 10% of damage dealt. Summons 3 untargetable Souls. Soul Max HP is 25% of your tankiest ally's Max HP. Souls deal basic magic damage + 5% of all damage currently being dealt by your team.
- **(6) The Lost Soul Zone:** Healing increased to 15%. Soul HP becomes 40%, echo becomes 8%. The arena floor turns "soully blue". Enemies in the zone require +15 Mana to cast abilities. Allies gain +2 Mana/sec. Enemies take 2% Max HP true damage per second; Allies heal 2% Max HP per second. Enemies below 5% HP are executed.
- **(8) The Underworld:** Healing increased to 25%. Soul HP becomes 60%, echo becomes 12%. Zone decay/heal increased to 4% Max HP/sec. Execute threshold increased to 10% HP.

### PROTECTOR (2)
- **(2)** All allies gain 10% Damage Amplification and 10% bonus Armor/MR. Protectors (Les, Lum) gain an additional 10% on top of this.


## 3. CHAMPION ROSTER (11 / 30)

### [ TIER 1 (1-Cost) ]

**ASTRA** (Najmi / Support)
- **Stats:** HP 500/900/1620 | Armor 20 | MR 20 | AD 25 | AP 20 | AS 0.65
- **Mana:** 0/60 | **Range:** 3
- **Ability (Constellation Link):** Tethers to highest DPS ally for 5s. 30% of damage the ally takes is redirected to Astra. Ally gains 20/30/50% bonus Attack Speed.

### [ TIER 2 (2-Cost) ]

**CYLA - "Eye of Hexa"** (Hexagon / Marksman)
- **Stats:** HP 90/140/180 | Armor 10/15/25 | MR 5/9/13 | AD 50/60/80 | AS 0.78
- **Mana:** 0/60 | **Range:** 4
- **Ability (Rocket Strike):** Can attack while casting. Fires a rocket dealing Physical Damage equal to 100% of her damage dealt in the last 2 seconds. Adjacent hexes take 25%.

**LUNIS - "The Crescent Shadow"** (Selini / Assassin)
- **Stats:** HP 550/990/1780 | Armor 25/35/50 | MR 25/35/50 | AD 45/68/102 | AS 0.80
- **Mana:** 0/40 | **Range:** 1
- **Passive:** Jumps to the farthest enemy at the start of combat and is invisible (untargetable) for the first 2 seconds.
- **Ability (Crescent Swipe):** Slashes in a cone dealing 150/225/350 Physical Damage. On kill, drops aggro for 1.5 seconds.

### [ TIER 3 (3-Cost) ]

**BAIRA - "Purple Sniper"** (Phaisa / Marksman)
- **Stats:** HP 100/150/200 | Armor 10/15/20 | MR 5/9/13 | AD 20/25/29 | AP 15/21/40 | AS 0.50
- **Mana:** 0 | **Range:** 4
- **Passive:** Applies Wound (30% healing reduction).
- **Ability:** Every 5th attack, burns a 3-hex radius for Magic Damage over time (scaling with AP). Gains 20/25/35% bonus Attack Speed for 5 seconds.

**FAIRE - "Arm of the Future"** (Hexagon / Mage)
- **Stats:** HP 225/333/427 | Armor 20/32/41 | MR 15/20/25 | AD 25/30/35 | AP 28/45/60 | AS 0.75
- **Mana:** 0/50 | **Range:** 3
- **Passive:** Attacks deal damage over 1 second (DoT). On death, instantly casts ability with current mana.
- **Ability:** Stuns target for 0.2s per 10 damage dealt to them recently. Deals Magic Damage. Adjacent enemies stunned for 60% duration.

**SOUL - "The Lost Vessel"** (Coregons / Bruiser)
- **Stats:** HP 100/150/200 | Shield 800/1200/1800 | Armor 40 | MR 40 | AD 45/68/102 | AS 0.70
- **Mana:** 0/70 | **Range:** 1
- **Passive:** Spawns with massive Shield but very low HP. Cannot be healed conventionally. Basic attacks restore 20/30/50 Shield.
- **Ability (Reap):** Deals 120/180/280 Physical Damage and permanently steals 15/20/30% of the target's Attack Damage and Armor for the rest of combat.

### [ TIER 4 (4-Cost) ]

**ALESK - "Protector of Helios"** (Helios / Tank)
- **Stats:** HP 325/475/650 | Armor 31/45/63 | MR 17/24/29 | AD 30/35/40 | AS 1.00
- **Mana:** 0/60 | **Range:** 1
- **Passive:** Gains 5/10/20% bonus Max HP, Armor, and MR from all sources.
- **Ability:** Gains a Shield of 7/15/30% Max HP. While active, reduces incoming damage by 5/12/25%.

**MYNA - "The Tidecaller"** (Selini / Support / Mage)
- **Stats:** HP 700/1260/2268 | Armor 30 | MR 30 | AD 30 | AP 40/60/90 | AS 0.70
- **Mana:** 0/80 | **Range:** 4
- **Ability (Ebb and Flow):** Creates a tidal pool that heals the lowest HP ally for 200/300/500 and deals 200/300/500 Magic Damage to the highest HP enemy inside it. Pool radius increases by 1 hex every cast.

### [ TIER 5 (5-Cost) ]

**LES - "Wall of Omnilium"** (Omnilium / Protector / Tank)
- **Stats:** HP 1000/2000/3500 | Armor 60/80/300 | MR 50/70/285 | AD 40/60/70 | AS 1.00
- **Mana:** 0/70 | **Range:** 1
- **Passive:** In Wall Mode, roots himself and heals for 10/10/20% of pre-mitigation damage taken.
- **Ability (Wall Mode):** Roots 3 closest enemies. Enters Wall Mode, gaining huge Armor/MR/HP and granting CC immunity to allies in his row.

**LUM - "Guardian of Omnilium"** (Omnilium / Protector / Bruiser)
- **Stats:** HP 600/1000/2500 | Armor 45/50/100 | MR 35/40/100 | AD 40/60/70 | AS 1.00
- **Mana:** 0/70 | **Range:** 1
- **Passive:** Starts combat with 15/30/100% bonus Armor, MR, and AD.
- **Ability:** Punches target dealing heavy Physical Damage. Knocks target back, stunning them and splashing Physical Damage to adjacent units.

**VEGA - "The Supernova"** (Najmi / Mage)
- **Stats:** HP 850/1530/2754 | Armor 35 | MR 35 | AD 40 | AP 80/120/180 | AS 0.75
- **Mana:** 0/100 | **Range:** 4
- **Ability (Channeling):** Channels for 4s. Every 0.5s, drops a star on random enemy for 100/150/400 Magic Damage. If completed, detonates a massive black hole dealing 500/800/2000 Magic Damage to the entire board.

## 4. ITEMS
EMBLEMS
- **COREGONS EMBLEM:** Recipe (Omnilium Seed + Mana Item). Grants +300 HP, +25 AD. Holder gains the Coregons Trait.


COMPONENT ITEMS:
omnilium’helmet (gives 8 armor + 15 Magic resist). 
selini’s water (gives +1 mana regen)
coregons sword (gives +15 ad)
omnilium vest (+25 armor)
phasia’s bow(+12 attack speed)
phaisa stick (+12 ability power)
Selinis Gloves (+15 critical chance)
omnilium heart(+180 health)

LEGENDARY ITEMS:
-OMNILIUM HELMET COMBINATIONS-
omniliums helmet + selinis water = sayona’s casket (user starts to combat with %20 mana,+2mana regen +13 armor +13 magic resist)

omniliums helmet + coregons sword = Soldiers’ Soul  (+25 ad, every third attack user grants 7 armor and 7 magic resist)

omniliums helmet + omnliums vest = Guardians Armor (40 armor+ 17 Magic Resist, Every 6th attack dealt to user grants him 6 armor and 8 magic resist)

omniliums helmet + phaisa’s bow = Gylachster (12 armor + 12 magic resist , when user hit 25th attack user became ghosted(no cc ))

omniliums helmet + phaisa’s stick = Mage Shield (12 armor + 30 Magic Resist,+12 Ability Power, when 4 abilities casted to user it creates a shield of users %10 maxhp , shield absors all the damage and dealt (stored damage+users %150 Ability power) as ability power to the target enemy. )

omniliums helmet + omniliums heart = Mother's Hands (+250 health + 12 armor + 12 magic resist, Grants User %10 bonus maxHP)

omniliums helmet+omniliums helmet = Big Helmet (+30 armor+30 magic resist, User get %50 less crit damage)

omniliums helmet + selinis gloves = Head Shot (+25 critical chance + 12mr + 12 armor + %10 damage amp )

-SELINI'S WATER COMBINATIONS-

selinis water + selinis water = Tear Of Mother (+4 mana regen + 15 ability power+15 attack damage, at second ability of user it grants %200AP magic shield for 5 seconds  if the user didnt get any damage grants user bonus %15 Ability power for 3 seconds.)

selinis water + coregons sword = Unalive Sword (+1 mana regen + 15 attack damage+15 ability power, every attack grants user bonus 4 mana)

selinis water + omniliums vest = Fishtank (+1 mana regen + 35 armor, every time user casts abilitiy grants bonus 7 armor)

selinis water + phaisas bow = Water Gun(+2 mana regen + 15 attack speed + 30 ability power, when user dealt any damage to target, target lose magic resist' %30 for 4 seconds)

selinis water+ phaisas stick = Divine Magic (+3mana regen + 30 ability power, every cast abilitiy grants user +1 mana regen and %3 ability power)

selinis water + omnlium heart = Blue Whale (+250 health + 2 mana regen, grants user %10 max hp as bonus hp, when users hp drops down below %30 grants it %30 MaxHP regen in 2 seconds + bonus +1 mana regen)

selinis water + selinis gloves = Fishscale (+25 critical chance + 3 mana regen, grant user bonus + %25 bite and %10max hp shield every ability cast)



-coregons sword combination-

coregons sword + coregons sword = Soul's Sword (grants user 45 Attack Damage, grants user bonus %20AD)

coregons sword + omnilium vest = Deadbeat(+25 armor + 25 attack damage, grants user a protective shield that ignores first ability that used on it)

coregons sword + phasias bow = Gunfire (+30 attack damage + 30 attack speed, every basic attack deals extra %0.5 MAXHP to target)

coregons sword + phasias stick = Electroblade (+30 attack damage + 30 ability power, every basic attack grants user %10damage dealt to heal him)

coregons sword + omnilium heart = HeartBroke (+30 attack damage + 250 health,when user drop down below %30hp grants user bonus %10 Attack Damage and %25 maxhp shield)

coregons sword + selinis gloves = Full Kit (+35 attack damage + %50 critical chance,grants user bonus %30 critical damage )


-phasias bow combinations-

phasias bow + phasias bow = twin snipers(+45 attack speed,grants user wound affects(reduce healing target by %30),Deals a percent of the target's maximum Health as true damage every second)

phasias bow + phasias stick = Phasia's Magic (+30 attack speed + 15 ability power, every attack grants user +1attack speed. limits at 60 stacks) 

phasias bow + omnilium heart = Betrayed Heart (+12 attack speed + 150 health +25 ability power + 1 mana regen, grants user's %5 of dealt damage to redealt to target's HP (ignores armor,shield,mr directly to HP) )

phasias bow + selinis glove = Guardian Destroyer (grants %25 critical chance + %15 attack damage + %20 attack speed , grants user ignore targets %30 armor(for 4 seconds))


-phasias stick combinations-

phasias stick+phasias stick= Magic Stick (+50 ability power, grants user bonus %30 ability power)
phasisas stick + omniliums heart = Omnilium's Book (+150 hp + 25 magic power, wounds enemy (reduce healing %30))
phasisas stick + selini gloves = Resist Puncher (+30 ability power, +25 % critical chance, grants user to critically damage their abilities)

